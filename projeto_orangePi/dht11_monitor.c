/*
 * Monitor do DHT11 por polling, com alerta por SMS (Twilio).
 *
 * - Lê o sensor a cada ~2 s e imprime temperatura e umidade até Ctrl+C.
 * - Quando a temperatura chega ao limite, dispara o script sms_alerta.py
 *   (uma vez por "subida": só rearma quando a temperatura cai HISTERESE_C
 *   abaixo do limite, e respeita um intervalo mínimo entre SMS).
 *
 * Compilar:  gcc -O2 -Wall -o dht11_monitor dht11_monitor.c -lgpiod
 * Executar:  sudo ./dht11_monitor [-t limite] [-c nucleo] [-n] [-v]
 *
 *   -t LIMITE  temperatura de alerta em °C (padrão 30.0)
 *   -c NUCLEO  núcleo da CPU para a leitura (padrão 3; -1 = sem afinidade)
 *   -n         não envia SMS (só imprime; bom para testar sem gastar mensagens)
 *   -v         modo detalhado (período de amostragem e bordas nas falhas)
 *
 * Requer libgpiod v2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <gpiod.h>
#include <grp.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHIP_PATH        "/dev/gpiochip0"
#define PINO             7            /* PA7, pino físico 29 */
#define NUCLEO_PADRAO    3
#define PRIORIDADE_RT    80

/* Caminhos usados para enviar o SMS (ajuste se mudar de lugar). */
#ifndef PYTHON_PADRAO
#define PYTHON_PADRAO    "/home/lse/venv-twilio/bin/python"
#endif
#ifndef SCRIPT_PADRAO
#define SCRIPT_PADRAO    "/home/lse/projetos/sms_alerta.py"
#endif
#ifndef ENV_PADRAO
#define ENV_PADRAO       "/home/lse/.twilio_env"
#endif

#define LIMITE_PADRAO_C  30.0
#define HISTERESE_C      2.0
#define COOLDOWN_SMS_NS  (INT64_C(600) * INT64_C(1000000000))   /* 10 min */
#define INTERVALO_NS     (INT64_C(2000000000))                  /* 2 s    */

#define START_BAIXO_NS   INT64_C(20000000)   /* 20 ms em nível baixo          */
#define PRAZO_CAPTURA_NS INT64_C(12000000)   /* quadro completo dura ~4,5 ms  */
#define SILENCIO_FIM_NS  INT64_C(300000)     /* linha parada => fim do quadro */
#define LIMIAR_BIT_NS    INT64_C(48000)      /* alto > 48 us => bit 1         */
#define MAX_BORDAS       160

typedef struct {
	int64_t t;      /* instante (CLOCK_MONOTONIC, ns) */
	int     nivel;  /* nível logo após a borda: 1 = subida, 0 = descida */
} borda_t;

typedef struct {
	double umidade;
	double temperatura;
	double periodo_us;   /* período médio de amostragem do polling */
} leitura_t;

enum {
	LEITURA_OK = 0,
	LEITURA_FALHA = -1,
	LEITURA_ERRO_GPIO = -2,
	LEITURA_INTERROMPIDA = 1,
};

static volatile sig_atomic_t executando = 1;

static void tratar_sinal(int sig)
{
	(void)sig;
	executando = 0;
}

static int64_t agora_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

/* Dorme até um instante absoluto; volta mais cedo se chegar Ctrl+C. */
static void dormir_ate_ns(int64_t alvo)
{
	struct timespec ts = {
		.tv_sec  = alvo / INT64_C(1000000000),
		.tv_nsec = alvo % INT64_C(1000000000),
	};
	while (executando &&
	       clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static const char *hora_atual(char *buf, size_t n)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, n, "%H:%M:%S", &tm);
	return buf;
}

static void configurar_tempo_real(int nucleo)
{
	if (nucleo >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(nucleo, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			perror("Aviso: sched_setaffinity");
	}

	struct sched_param sp = { .sched_priority = PRIORIDADE_RT };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
		perror("Aviso: SCHED_FIFO (precisa de root)");

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		perror("Aviso: mlockall");
}

/*
 * Captura uma rajada de bordas.
 * Retorna 0 se capturou, -1 em erro de GPIO, 1 se interrompida por Ctrl+C.
 */
static int capturar(struct gpiod_line_request *req,
		    struct gpiod_line_config *cfg_saida,
		    struct gpiod_line_config *cfg_entrada,
		    borda_t *b, unsigned *n_bordas,
		    uint64_t *amostras, int64_t *duracao_ns)
{
	unsigned n = 0;

	/* Start: linha em nível baixo por 20 ms. */
	if (gpiod_line_request_reconfigure_lines(req, cfg_saida) != 0)
		return -1;
	dormir_ate_ns(agora_ns() + START_BAIXO_NS);
	if (!executando)
		return 1;

	/* Solta a linha (entrada) e começa a amostrar imediatamente. */
	if (gpiod_line_request_reconfigure_lines(req, cfg_entrada) != 0)
		return -1;

	int ultimo = gpiod_line_request_get_value(req, PINO);
	if (ultimo < 0)
		return -1;

	int64_t t_ini = agora_ns();
	int64_t t_ultima_borda = t_ini;
	int64_t prazo = t_ini + PRAZO_CAPTURA_NS;
	uint64_t cont = 1;
	int64_t t = t_ini;

	for (;;) {
		int v = gpiod_line_request_get_value(req, PINO);
		t = agora_ns();
		cont++;
		if (v < 0)
			return -1;

		if (v != ultimo) {
			if (n < MAX_BORDAS) {
				b[n].t = t;
				b[n].nivel = v;
				n++;
			}
			ultimo = v;
			t_ultima_borda = t;
		}

		if (t >= prazo)
			break;
		if (n >= 80 && (t - t_ultima_borda) > SILENCIO_FIM_NS)
			break;
	}

	*n_bordas = n;
	*amostras = cont;
	*duracao_ns = t - t_ini;
	return 0;
}

/*
 * Procura a resposta do sensor (subida seguida de descida 60..110 us depois)
 * e lê os 40 bits pela duração do nível alto de cada um.
 */
static int decodificar(const borda_t *b, unsigned n, unsigned char dados[5],
		       const char **motivo)
{
	int inicio = -1;

	for (unsigned i = 0; i + 1 < n; i++) {
		if (b[i].nivel == 1 && b[i + 1].nivel == 0) {
			int64_t d = b[i + 1].t - b[i].t;
			if (d >= 60000 && d <= 110000) {
				inicio = (int)i;
				break;
			}
		}
	}
	if (inicio < 0) {
		*motivo = "resposta do sensor (~80 us alto) não encontrada";
		return -1;
	}
	if ((unsigned)inicio + 82 > n) {
		*motivo = "bordas insuficientes para 40 bits";
		return -1;
	}

	for (int i = 0; i < 5; i++)
		dados[i] = 0;

	for (int k = 0; k < 40; k++) {
		const borda_t *r = &b[inicio + 2 + 2 * k];
		const borda_t *f = &b[inicio + 3 + 2 * k];
		if (r->nivel != 1 || f->nivel != 0) {
			*motivo = "bordas fora de ordem";
			return -1;
		}
		int64_t alto = f->t - r->t;
		if (alto < 10000 || alto > 110000) {
			*motivo = "pulso de bit fora da faixa";
			return -1;
		}
		unsigned bit = alto > LIMIAR_BIT_NS;
		dados[k / 8] = (unsigned char)((dados[k / 8] << 1) | bit);
	}
	return 0;
}

static void imprimir_diagnostico(const borda_t *b, unsigned n)
{
	fprintf(stderr, "Bordas capturadas: %u\n", n);
	for (unsigned i = 1; i < n; i++) {
		fprintf(stderr, "  %3u %-6s intervalo=%.1f us\n", i,
			b[i].nivel ? "SUBIDA" : "DESCIDA",
			(b[i].t - b[i - 1].t) / 1000.0);
	}
}

/* Uma leitura completa: start, captura, decodificação e checksum. */
static int ler_dht11(struct gpiod_line_request *req,
		     struct gpiod_line_config *cfg_saida,
		     struct gpiod_line_config *cfg_entrada,
		     bool verboso, leitura_t *saida, const char **erro)
{
	borda_t bordas[MAX_BORDAS];
	unsigned n = 0;
	uint64_t amostras = 0;
	int64_t dur = 0;
	unsigned char d[5];

	int r = capturar(req, cfg_saida, cfg_entrada, bordas, &n, &amostras, &dur);
	if (r < 0) {
		*erro = "erro de GPIO";
		return LEITURA_ERRO_GPIO;
	}
	if (r > 0)
		return LEITURA_INTERROMPIDA;

	if (decodificar(bordas, n, d, erro) != 0) {
		if (verboso)
			imprimir_diagnostico(bordas, n);
		return LEITURA_FALHA;
	}

	unsigned char soma = (unsigned char)(d[0] + d[1] + d[2] + d[3]);
	if (soma != d[4]) {
		*erro = "checksum incorreto";
		if (verboso)
			imprimir_diagnostico(bordas, n);
		return LEITURA_FALHA;
	}

	saida->umidade = d[0] + d[1] / 10.0;
	saida->temperatura = d[2] + d[3] / 10.0;
	saida->periodo_us = amostras ? dur / 1000.0 / (double)amostras : 0.0;
	return LEITURA_OK;
}

/*
 * Dispara o script Python em um processo filho, sem esperar por ele.
 * O filho NÃO herda o modo tempo real nem o núcleo reservado, para não
 * atrapalhar as próximas leituras, e deixa de ser root se veio de sudo.
 */
static int disparar_sms(double temp, double umid, int nucleo)
{
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid > 0)
		return 0;

	/* ---- processo filho ---- */
	setpgid(0, 0);            /* Ctrl+C no terminal não mata o envio */
	signal(SIGCHLD, SIG_DFL); /* o "ignorar" do pai seria herdado no exec */

	struct sched_param sp = { .sched_priority = 0 };
	sched_setscheduler(0, SCHED_OTHER, &sp);

	cpu_set_t set;
	CPU_ZERO(&set);
	long ncpu = sysconf(_SC_NPROCESSORS_CONF);
	for (long i = 0; i < ncpu && i < CPU_SETSIZE; i++)
		if (i != nucleo)
			CPU_SET((int)i, &set);
	if (CPU_COUNT(&set) == 0)
		for (long i = 0; i < ncpu && i < CPU_SETSIZE; i++)
			CPU_SET((int)i, &set);
	sched_setaffinity(0, sizeof(set), &set);

	const char *su = getenv("SUDO_UID");
	const char *sg = getenv("SUDO_GID");
	if (getuid() == 0 && su && sg) {
		uid_t u = (uid_t)strtoul(su, NULL, 10);
		gid_t g = (gid_t)strtoul(sg, NULL, 10);
		if (setgroups(0, NULL) != 0 || setgid(g) != 0 || setuid(u) != 0) {
			perror("Erro ao largar privilégios de root");
			_exit(127);
		}
	}

	char tb[32], ub[32];
	snprintf(tb, sizeof(tb), "%.1f", temp);
	snprintf(ub, sizeof(ub), "%.1f", umid);
	execl(PYTHON_PADRAO, PYTHON_PADRAO, SCRIPT_PADRAO, ENV_PADRAO,
	      tb, ub, (char *)NULL);
	perror("Erro ao executar o script de SMS");
	_exit(127);
}

static void uso(const char *prog)
{
	fprintf(stderr,
		"Uso: %s [-t limite_C] [-c nucleo] [-n] [-v]\n"
		"  -t  temperatura de alerta (padrão %.1f)\n"
		"  -c  núcleo da CPU (padrão %d; -1 = sem afinidade)\n"
		"  -n  não envia SMS\n"
		"  -v  modo detalhado\n",
		prog, LIMITE_PADRAO_C, NUCLEO_PADRAO);
}

int main(int argc, char **argv)
{
	double limite = LIMITE_PADRAO_C;
	int nucleo = NUCLEO_PADRAO;
	bool enviar_sms = true;
	bool verboso = false;
	int opt;

	while ((opt = getopt(argc, argv, "t:c:nvh")) != -1) {
		char *fim = NULL;
		switch (opt) {
		case 't':
			limite = strtod(optarg, &fim);
			if (fim == optarg || *fim != '\0') {
				uso(argv[0]);
				return 2;
			}
			break;
		case 'c':
			nucleo = atoi(optarg);
			break;
		case 'n':
			enviar_sms = false;
			break;
		case 'v':
			verboso = true;
			break;
		default:
			uso(argv[0]);
			return 2;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (enviar_sms) {
		if (access(PYTHON_PADRAO, X_OK) != 0)
			fprintf(stderr, "Aviso: python não encontrado em %s\n", PYTHON_PADRAO);
		if (access(SCRIPT_PADRAO, R_OK) != 0)
			fprintf(stderr, "Aviso: script não encontrado em %s\n", SCRIPT_PADRAO);
		if (access(ENV_PADRAO, R_OK) != 0)
			fprintf(stderr, "Aviso: credenciais não encontradas em %s\n", ENV_PADRAO);
	}

	unsigned int offset = PINO;
	int ret = 0;

	struct gpiod_chip *chip = NULL;
	struct gpiod_line_settings *set_saida = NULL, *set_entrada = NULL;
	struct gpiod_line_config *cfg_saida = NULL, *cfg_entrada = NULL;
	struct gpiod_request_config *req_cfg = NULL;
	struct gpiod_line_request *req = NULL;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tratar_sinal;   /* sem SA_RESTART: acorda o sleep */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGCHLD, SIG_IGN);       /* filhos (envio de SMS) não viram zumbis */

	configurar_tempo_real(nucleo);

	chip = gpiod_chip_open(CHIP_PATH);
	if (!chip) {
		perror("Erro ao abrir o controlador de gpio");
		return 1;
	}

	set_saida = gpiod_line_settings_new();
	set_entrada = gpiod_line_settings_new();
	cfg_saida = gpiod_line_config_new();
	cfg_entrada = gpiod_line_config_new();
	req_cfg = gpiod_request_config_new();
	if (!set_saida || !set_entrada || !cfg_saida || !cfg_entrada || !req_cfg) {
		perror("Erro de alocação");
		ret = 1;
		goto fim;
	}

	gpiod_line_settings_set_direction(set_saida, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(set_saida, GPIOD_LINE_VALUE_INACTIVE);
	gpiod_line_settings_set_direction(set_entrada, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_edge_detection(set_entrada, GPIOD_LINE_EDGE_NONE);

	if (gpiod_line_config_add_line_settings(cfg_saida, &offset, 1, set_saida) != 0 ||
	    gpiod_line_config_add_line_settings(cfg_entrada, &offset, 1, set_entrada) != 0) {
		perror("Erro ao configurar a linha");
		ret = 1;
		goto fim;
	}

	gpiod_request_config_set_consumer(req_cfg, "dht11-monitor");
	req = gpiod_chip_request_lines(chip, req_cfg, cfg_entrada);
	if (!req) {
		perror("Erro ao solicitar a linha");
		ret = 1;
		goto fim;
	}

	printf("Monitor DHT11 em %s, offset %u (núcleo %d).\n", CHIP_PATH, offset, nucleo);
	printf("Alerta acima de %.1f °C (%s). Ctrl+C para encerrar.\n",
	       limite, enviar_sms ? "SMS ligado" : "SMS desligado (-n)");
	printf("Aguardando 2 segundos para estabilização do sensor...\n");
	dormir_ate_ns(agora_ns() + INTERVALO_NS);

	bool alertado = false;       /* já avisou nesta "subida" */
	bool ja_enviou = false;
	int64_t ultimo_sms = 0;
	unsigned falhas = 0;

	while (executando) {
		leitura_t l;
		const char *erro = "";
		char hora[16];

		int r = ler_dht11(req, cfg_saida, cfg_entrada, verboso, &l, &erro);
		if (r == LEITURA_INTERROMPIDA)
			break;
		if (r == LEITURA_ERRO_GPIO) {
			fprintf(stderr, "[%s] %s: %s\n", hora_atual(hora, sizeof(hora)),
				erro, strerror(errno));
			ret = 1;
			break;
		}

		if (r == LEITURA_OK) {
			falhas = 0;
			printf("[%s] Umidade: %.1f %%RH | Temperatura: %.1f °C",
			       hora_atual(hora, sizeof(hora)), l.umidade, l.temperatura);
			if (verboso)
				printf(" | amostragem: %.2f us", l.periodo_us);
			printf("\n");

			int64_t agora = agora_ns();
			if (l.temperatura >= limite) {
				bool pode = !ja_enviou || (agora - ultimo_sms) >= COOLDOWN_SMS_NS;
				if (!alertado && pode) {
					if (!enviar_sms) {
						printf("[%s] ALERTA: %.1f °C >= %.1f °C (SMS desligado).\n",
						       hora, l.temperatura, limite);
						alertado = true;
					} else if (disparar_sms(l.temperatura, l.umidade, nucleo) == 0) {
						printf("[%s] ALERTA: %.1f °C >= %.1f °C. Enviando SMS...\n",
						       hora, l.temperatura, limite);
						alertado = true;
						ja_enviou = true;
						ultimo_sms = agora;
					}
				}
			} else if (l.temperatura <= limite - HISTERESE_C) {
				if (alertado)
					printf("[%s] Temperatura normalizada (%.1f °C).\n",
					       hora, l.temperatura);
				alertado = false;
			}
		} else {
			falhas++;
			fprintf(stderr, "[%s] Falha na leitura: %s (falhas seguidas: %u)\n",
				hora_atual(hora, sizeof(hora)), erro, falhas);
		}

		dormir_ate_ns(agora_ns() + INTERVALO_NS);   /* DHT11: >= ~2 s entre leituras */
	}

	printf("\nEncerrando...\n");

fim:
	if (req)
		gpiod_line_request_release(req);
	if (req_cfg)
		gpiod_request_config_free(req_cfg);
	if (cfg_entrada)
		gpiod_line_config_free(cfg_entrada);
	if (cfg_saida)
		gpiod_line_config_free(cfg_saida);
	if (set_entrada)
		gpiod_line_settings_free(set_entrada);
	if (set_saida)
		gpiod_line_settings_free(set_saida);
	if (chip)
		gpiod_chip_close(chip);
	return ret;
}
