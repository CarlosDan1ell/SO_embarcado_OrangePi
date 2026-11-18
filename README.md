# Monitor de temperatura e umidade com DHT11 e alerta por SMS

Projeto da disciplina de Sistemas Embarcados, executado em uma **Orange Pi PC (Allwinner H3)** com sensor **DHT11**.

**Equipe:** _Nome 1, Nome 2, Nome 3_

## 1. Objetivo do projeto

Ler temperatura e umidade de um sensor DHT11 em tempo contínuo, mostrar as leituras no terminal e **enviar um SMS quando a temperatura chegar a um limite** (padrão: 30 °C). O programa roda até o usuário apertar `Ctrl+C`, quando encerra liberando o pino.

O DHT11 usa um protocolo de um fio com pulsos de 23 a 120 µs. O projeto lê esses pulsos por **polling** (leitura direta do nível do pino com timestamp) e delega o envio do SMS a um **processo filho**, para que o envio não atrase as leituras seguintes.

## 2. Hardware

| Item | Detalhe |
|---|---|
| Placa | Orange Pi PC (H3), Armbian |
| Sensor | DHT11 (faixa de 0 a 50 °C, leitura a cada 2 s ou mais) |
| Pino de dados | PA7 (`/dev/gpiochip0`, offset 7, pino físico 29) |
| Alimentação | 3,3 V e GND |
| Pull-up | Resistor de 4,7 a 10 kΩ entre o DATA e 3,3 V (módulos de 3 pinos já trazem) |

## 3. Arquitetura

```
 dht11_monitor (C)                                   sms_alerta.py (Python)
 ┌──────────────────────────────────────┐            ┌──────────────────────┐
 │ laço principal (a cada ~2 s)         │            │ lê credenciais       │
 │   1. start: pino em baixo por 20 ms  │  fork()    │ chama a API Twilio   │
 │   2. polling: captura as bordas      │  + exec()  │ imprime o resultado  │
 │   3. decodifica 40 bits + checksum   │ ─────────► └──────────┬───────────┘
 │   4. imprime umidade e temperatura   │  argv:                │ HTTPS
 │   5. se temp >= limite: dispara SMS  │  arquivo .env,        ▼
 └──────────────────────────────────────┘  temp, umidade     Twilio ──► SMS
```

- **Captura:** o programa reconfigura o pino de saída (baixo) para entrada e lê o nível em laço, guardando o instante de cada mudança (`CLOCK_MONOTONIC`).
- **Decodificação:** acha a resposta do sensor (~80 µs em alto) e lê os 40 bits pela duração do nível alto de cada um (acima de 48 µs é bit 1). O último byte é o checksum.
- **Alerta:** o SMS sai uma vez por "subida" de temperatura. O alerta só rearma quando a temperatura cai 2 °C abaixo do limite, e há um intervalo mínimo de 10 minutos entre mensagens.

### Por que polling e não eventos de borda

A primeira versão usava detecção de bordas do libgpiod. A captura com um analisador lógico mostrou que o sinal do sensor estava correto, mas o kernel entregava só uma parte das bordas, porque a latência da thread de IRQ é maior que a duração dos pulsos. O polling não depende de IRQ e resolveu o problema na maior parte das leituras.

Tempos medidos na captura do analisador lógico (24 MHz) e comparados com o esperado:

| Fase | Esperado | Medido |
|---|---|---|
| Resposta do sensor, baixo | ~80 µs | 82,1 µs |
| Resposta do sensor, alto | ~80 µs | 84,6 µs |
| Baixo antes de cada bit | ~50 µs | 52,3 a 52,7 µs |
| Alto de um bit 0 | 26 a 28 µs | 23,5 µs |
| Alto de um bit 1 | ~70 µs | 69,3 µs |

## 4. Mecanismos de concorrência, IPC e sincronização

| Mecanismo | Onde | Para que serve |
|---|---|---|
| Processos: `fork()` + `execl()` | `disparar_sms()` em `dht11_monitor.c` | O filho executa o script Python e o pai continua lendo o sensor sem esperar o envio |
| `setpgid()` no filho | `disparar_sms()` | O filho fica em outro grupo de processos, então o `Ctrl+C` no terminal não interrompe um envio em andamento |
| Sinais: `sigaction` (`SIGINT`, `SIGTERM`) | `main()` | Encerramento limpo. O handler só muda uma flag `volatile sig_atomic_t`, e o laço sai e libera o pino |
| `SIGCHLD` ignorado | `main()` | O kernel recolhe o filho sozinho, sem processos zumbis |
| Passagem de dados por argumentos do `exec` | `disparar_sms()` | O pai entrega ao filho o caminho do arquivo de credenciais, a temperatura e a umidade |
| `clock_nanosleep` com tempo absoluto | `dormir_ate_ns()` | Espera de 20 ms do start e de 2 s entre leituras, interrompível pelo sinal |
| Escalonamento: `sched_setscheduler(SCHED_FIFO)`, `sched_setaffinity`, `mlockall` | `configurar_tempo_real()` | Tentam reduzir o atraso do polling. O filho volta para `SCHED_OTHER` e sai do núcleo reservado, para não competir com o laço de leitura |
| Perda de privilégios: `setgroups`/`setgid`/`setuid` | `disparar_sms()` | Se o programa foi iniciado com `sudo`, o filho que fala com a rede deixa de ser root |

### Em desenvolvimento

- **Threads POSIX (`pthread`):** já existe um `led.c` com duas threads que piscam um LED vermelho e um verde, com encerramento por `Ctrl+C`. A integração com o monitor, para dar feedback visual do estado do sensor, ainda não foi feita.

## 5. Estrutura do repositório

```
monitoramento/
├── dht11_monitor.c   # programa principal: leitura contínua + alerta
├── sms_alerta.py     # envio do SMS pelo Twilio (chamado pelo programa em C)
├── dht11_polling.c   # versão de leitura única, usada para validar o polling
└── README.md
```

## 6. Requisitos

- Orange Pi PC com Linux (Armbian) e acesso a `/dev/gpiochip0`
- `gcc` e **libgpiod 2.x** com cabeçalhos (a API v2: `gpiod_line_config`, `gpiod_line_request_get_value`)
- Python 3 com o pacote `twilio`
- Conta no Twilio com um número de origem

```bash
sudo apt install gcc libgpiod-dev python3-venv
pkg-config --modversion libgpiod        # deve mostrar 2.x
```

## 7. Como executar

### 7.1 Ambiente Python

```bash
python3 -m venv ~/venv-twilio
source ~/venv-twilio/bin/activate
pip install twilio
```

### 7.2 Credenciais

Crie `~/.twilio_env` com as quatro variáveis e restrinja a permissão:

```bash
export TWILIO_ACCOUNT_SID="AC..."
export TWILIO_AUTH_TOKEN="..."
export TWILIO_FROM="+1..."      # número do Twilio
export TWILIO_TO="+55..."       # celular que recebe o alerta
```

```bash
chmod 600 ~/.twilio_env
```

> **Nunca coloque esse arquivo no repositório.** Ele contém o token da conta. Se for usar git, inclua `.twilio_env` no `.gitignore`. Cada variável deve aparecer uma única vez no arquivo, e o script usa o primeiro valor que encontrar.

### 7.3 Compilar

```bash
cd monitoramento
gcc -O2 -Wall -o dht11_monitor dht11_monitor.c -lgpiod \
    -DSCRIPT_PADRAO='"/home/USUARIO/projetos/monitoramento/sms_alerta.py"' \
    -DPYTHON_PADRAO='"/home/USUARIO/venv-twilio/bin/python"' \
    -DENV_PADRAO='"/home/USUARIO/.twilio_env"'
```

Troque `USUARIO` pelo usuário da placa. Os três caminhos têm valores padrão no topo do `.c`, então só precisam ser passados se forem diferentes do padrão (`/home/lse/...`).

### 7.4 Rodar

Precisa de `sudo` para o modo tempo real e para o acesso ao GPIO.

```bash
sudo ./dht11_monitor -n        # só imprime, não envia SMS (bom para testar)
sudo ./dht11_monitor           # alerta por SMS acima de 30 °C
sudo ./dht11_monitor -t 35     # outro limite
```

| Opção | Efeito |
|---|---|
| `-t LIMITE` | Temperatura de alerta em °C (padrão 30,0) |
| `-c NUCLEO` | Núcleo da CPU usado na leitura (padrão 3; `-1` desliga a afinidade) |
| `-n` | Não envia SMS |
| `-v` | Modo detalhado: período médio de amostragem e bordas capturadas nas falhas |

Para encerrar, aperte `Ctrl+C`.

### 7.5 Exemplo de saída

Com `-t 20` para forçar o alerta em um ambiente de ~22 °C:

```
Monitor DHT11 em /dev/gpiochip0, offset 7 (núcleo 3).
Alerta acima de 20.0 °C (SMS ligado). Ctrl+C para encerrar.
[15:38:27] Umidade: 38.0 %RH | Temperatura: 22.0 °C
[15:38:27] ALERTA: 22.0 °C >= 20.0 °C. Enviando SMS...
SMS enviado: SM... queued
[15:38:31] Falha na leitura: pulso de bit fora da faixa (falhas seguidas: 1)
[15:38:33] Umidade: 38.0 %RH | Temperatura: 22.0 °C
```

## 8. Conta de teste do Twilio

O projeto foi testado com uma conta de teste do Twilio, que tem restrições:

- só envia SMS para **números verificados** na conta;
- só aceita **modelos de mensagem prontos**: o SMS chega com o texto do modelo `sms_internal_alerts`, sem a temperatura e a umidade;
- tem um limite de mensagens gratuitas (100 na conta usada).

Depois do upgrade da conta, basta trocar `CONTA_DE_TESTE = True` para `False` no `sms_alerta.py`, e a mensagem passa a levar a temperatura e a umidade.

## 9. Limitações conhecidas

- **Falhas de leitura:** o polling perde ou atrasa algumas bordas, e uma parte das leituras falha (checksum incorreto, bordas insuficientes ou pulso fora da faixa). O programa tenta de novo a cada 2 s e a temperatura muda devagar, então o alerta continua funcionando.
- **Tempo real:** na placa usada, o `SCHED_FIFO` foi recusado mesmo com `sudo`, e o programa segue com prioridade normal. A causa ainda não foi investigada.
- **Sensor:** o DHT11 mede de 0 a 50 °C. Ao testar com ar quente, mantenha distância para não danificar o sensor.

## 10. Próximos passos

- Integrar o `led.c` (threads) como indicador visual do estado do sistema.
- Investigar o `SCHED_FIFO` recusado e reduzir a taxa de falhas de leitura.
- Enviar a temperatura e a umidade na notificação, com ntfy ou depois do upgrade do Twilio.
