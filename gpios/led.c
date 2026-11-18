#include <stdio.h>
#include <unistd.h>
#include <gpiod.h>
#include <signal.h>

volatile sig_atomic_t sinal = 1;
typedef struct {

        unsigned int gpio;
        unsigned int intervalo;
        struct gpiod_line_request *request;

}led_t;




void handler (int sig){
		(void)sig;
		printf("encerrando...\n");
		sinal = 0;
	}

void *piscar_led1(void *arg){

	led_t *config = arg;
	config->intervalo = 1;
	config->gpio = 6;

	while(sinal){
	        gpiod_line_request_set_value(config->request,config->gpio,GPIOD_LINE_VALUE_ACTIVE);
	        sleep(config->intervalo);
		gpiod_line_request_set_value(config->request,config->gpio,GPIOD_LINE_VALUE_INACTIVE); 
      		sleep(config->intervalo);
     	 }
	return NULL;

}
void *piscar_led2(void *arg){

	led_t *config = arg;
        config->intervalo = 3;
        config->gpio = 68;

        while(sinal){
                gpiod_line_request_set_value(config->request,config->gpio,GPIOD_LINE_VALUE_ACTIVE);
                sleep(config->intervalo);
                gpiod_line_request_set_value(config->request,config->gpio,GPIOD_LINE_VALUE_INACTIVE); 
                sleep(config->intervalo);
         }
	return NULL;

}


int main(void)
{
    struct gpiod_chip *chip;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_cfg;
    struct gpiod_line_config *line_cfg2;

    struct gpiod_line_request *request;
    struct gpiod_line_request *request2;
    pthread_t t1,t2;
    led_t led1,led2;
    unsigned int offset1 = 6; // PA6 - LED1
   unsigned int offset2 = 68 //PC4 - LED2
    unsigned int offset[2] = {6,68};
    // 1. Abrir o controlador GPIO
    chip = gpiod_chip_open("/dev/gpiochip0");
    if (chip == NULL) {
        perror("Erro ao abrir o controlador GPIO");
        return 1;
    }

    // 2. Criar as configurações da linha
    settings = gpiod_line_settings_new();

    if (settings == NULL) {
        perror("Erro ao criar settings");
        gpiod_chip_close(chip);
        return 1;
    }

    // 3. Configurar a linha como saída
    if (gpiod_line_settings_set_direction(
            settings, GPIOD_LINE_DIRECTION_OUTPUT) == -1) {

        perror("Erro ao configurar direção");

        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }

    // 4. Criar a configuração das linhas
    line_cfg = gpiod_line_config_new();
    line_cfg2 = gpiod_line_config_new();

    if (line_cfg == NULL||line_cfg2==NULL) {
        perror("Erro ao criar line_cfgs");

        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }

    // 5. Associar PA6 às configurações
    if (gpiod_line_config_add_line_settings(
            line_cfg, &offset1, 1, settings) == -1) {

        perror("Erro ao adicionar a linha1");

        gpiod_line_config_free(line_cfg1);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }
	if (gpiod_line_config_add_line_settings(
            line_cfg2, &offset, 1, settings) == -1) {

        perror("Erro ao adicionar a linha2");

        gpiod_line_config_free(line_cfg2);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }


    // 6. Solicitar controle da linha ao kernel
    request = gpiod_chip_request_lines(chip, NULL, line_cfg);
    request2 = gpiod_chip_request_lines(chip, NULL, line_cfg2);
    if (request == NULL||request2==NULL) {
        perror("Erro ao solicitar linha GPIO");

        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }
    signal(SIGINT,handler);
    led1.request = request;
    led2.request = request2;
    pthread_create(&t1,NULL,piscar_led1,&led1);
    pthread_create(&t2,NULL,piscar_led2,&led2);

    if(pthread_join(t1,NULL)!=0){
	perror("erro ao criar task1");
	gpiod_line_request_release(request);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
	}

    if(pthread_join(t2,NULL)!=0){
        perror("erro ao criar task1");
        gpiod_line_request_release(request);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        }

//    while(sinal){
//	gpiod_line_request_set_value(request,offset,GPIOD_LINE_VALUE_ACTIVE);
//	sleep(1);
//	gpiod_line_request_set_value(request,offset,GPIOD_LINE_VALUE_INACTIVE);
//	sleep(1);
//	}

    gpiod_line_request_set_value(request,6,GPIOD_LINE_VALUE_INACTIVE);
    gpiod_line_request_set_value(request2,68,GPIOD_LINE_VALUE_INACTIVE);

    // 7. Liberar os recursos
    gpiod_line_request_release(request);
    gpiod_line_config_free(line_cfg);
    gpiod_line_request_release(request2);
    gpiod_line_config_free(line_cfg2);

    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);

    return 0;
}
