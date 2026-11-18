#!/usr/bin/env python3
"""Envia o SMS de alerta pelo Twilio.

Uso: sms_alerta.py ARQUIVO_ENV TEMPERATURA UMIDADE

O ARQUIVO_ENV (por exemplo ~/.twilio_env) deve ter linhas como:
    export TWILIO_ACCOUNT_SID="AC..."
    export TWILIO_AUTH_TOKEN="..."
    export TWILIO_FROM="+1..."      # número do Twilio
    export TWILIO_TO="+55..."       # seu celular

Variáveis que já estiverem no ambiente têm prioridade sobre o arquivo.
"""
import os
import sys

from twilio.rest import Client

# Conta de teste: só aceita nomes de modelos prontos no corpo.
# Depois do upgrade, troque para False para enviar texto com os valores.
CONTA_DE_TESTE = True
MODELO_TESTE = "sms_internal_alerts"


def carregar_env(caminho):
    try:
        with open(caminho, encoding="utf-8") as f:
            for linha in f:
                linha = linha.strip()
                if not linha or linha.startswith("#"):
                    continue
                if linha.startswith("export "):
                    linha = linha[len("export "):]
                chave, sep, valor = linha.partition("=")
                if sep:
                    valor = valor.strip().strip('"').strip("'")
                    os.environ.setdefault(chave.strip(), valor)
    except OSError as e:
        print(f"Aviso: não foi possível ler {caminho}: {e}", file=sys.stderr)


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2

    arquivo_env, temperatura, umidade = sys.argv[1:4]
    carregar_env(arquivo_env)

    try:
        sid = os.environ["TWILIO_ACCOUNT_SID"]
        token = os.environ["TWILIO_AUTH_TOKEN"]
        origem = os.environ["TWILIO_FROM"]
        destino = os.environ["TWILIO_TO"]
    except KeyError as e:
        print(f"Falta a variável {e} no ambiente ou em {arquivo_env}", file=sys.stderr)
        return 1

    if CONTA_DE_TESTE:
        corpo = MODELO_TESTE
    else:
        corpo = f"Alerta: temperatura {temperatura} C, umidade {umidade} %"

    try:
        msg = Client(sid, token).messages.create(
            from_=origem, to=destino, body=corpo
        )
    except Exception as e:  # erros da API trazem código e mensagem do Twilio
        print(f"Falha ao enviar SMS: {e}", file=sys.stderr)
        return 1

    print(f"SMS enviado: {msg.sid} {msg.status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
