PROJ=MONITOR_MESH
LIN=$(shell grep '^platform =' platformio.ini)
PLATFORM=$(shell echo ${LIN} | cut -d "=" -f2- | tr -d '[:space:]' )

BRANCH_NAME=main
DEST=pharmdata.ddns.net
SRCDIR=wemos_d1_mini32


all: mkdir push

mu: upload monitor

mkdir:
	ssh ${DEST} "mkdir -p /var/www/html/firmware/${PLATFORM}/${PROJ}/${BRANCH_NAME}"

push:
	echo "PROJ: ${PROJ}"
	echo "LIN: ${LIN}"
	echo "PLATFORM: ${PLATFORM}"
	pio run
	scp .pio/build/${SRCDIR}/firmware.bin ${DEST}:/var/www/html/firmware/${PLATFORM}/${PROJ}/${BRANCH_NAME}/firmware.bin

upload:
	pio run --target upload

monitor:
	pio device monitor

clean:
	pio run --target clean




