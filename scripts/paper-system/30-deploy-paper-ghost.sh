#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
VM1_PORT=${VM1_PORT:-2401}
SSH_USER=${SSH_USER:-ubuntu}
SSH_HOST=${SSH_HOST:-127.0.0.1}
IMAGE=${IMAGE:-cxlfork-ghost:paper-v1}
CONTAINER=${CONTAINER:-cxlfork-ghost-paper}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2501}

PORTER=$WS/cxlporter
RUNTIME=$PORTER/cxlfork-capsule-runtime
SSH=(ssh -o BatchMode=yes -o StrictHostKeyChecking=no
	-o UserKnownHostsFile=/dev/null -p "$VM1_PORT"
	"$SSH_USER@$SSH_HOST")
SCP=(scp -q -o BatchMode=yes -o StrictHostKeyChecking=no
	-o UserKnownHostsFile=/dev/null -P "$VM1_PORT")

make -C "$PORTER" \
	cxlporter-paper cxlfork-ghost-agent cxlfork-capsule-runtime

"${SSH[@]}" 'install -d -m 0755 /tmp/cxlfork-ghost-build'
"${SCP[@]}" \
	"$PORTER/cxlfork-ghost-agent" \
	"$PORTER/cxlporter-paper" \
	"$RUNTIME" \
	"$PORTER/docker/Dockerfile.ghost" \
	"$SSH_USER@$SSH_HOST:/tmp/cxlfork-ghost-build/"

"${SSH[@]}" "set -e
	cd /tmp/cxlfork-ghost-build
	sudo docker build -f Dockerfile.ghost -t '$IMAGE' .
	sudo install -d -m 0755 /run/cxlfork-paper
	sudo docker rm -f '$CONTAINER' >/dev/null 2>&1 || true
	sudo docker run -d --name '$CONTAINER' --restart unless-stopped \
		--network none \
		--device /dev/cxlfork:/dev/cxlfork \
		-v /run/cxlfork-paper:/run/cxlfork \
		'$IMAGE'
	for i in \$(seq 1 30); do
		test -S /run/cxlfork-paper/ghost.sock && break
		sleep 1
	done
	test -S /run/cxlfork-paper/ghost.sock
	sudo docker inspect --format \
		'{{range .HostConfig.Devices}}{{.PathOnHost}}:{{.PathInContainer}}{{println}}{{end}}' \
		'$CONTAINER' | grep -q '^/dev/cxlfork:/dev/cxlfork'
	sudo install -m 0755 cxlporter-paper /usr/local/bin/cxlporter-paper
	printf '%s\n' \
		'[Unit]' \
		'Description=CXLfork paper CXLP/2 control daemon' \
		'After=docker.service' \
		'Requires=docker.service' \
		'' \
		'[Service]' \
		'Type=simple' \
		'User=ubuntu' \
		'ExecStart=/usr/local/bin/cxlporter-paper daemon 0.0.0.0 7100 /run/cxlfork-paper/ghost.sock' \
		'Restart=on-failure' \
		'RestartSec=1' \
		'' \
		'[Install]' \
		'WantedBy=multi-user.target' |
		sudo tee /etc/systemd/system/cxlporter-paper.service >/dev/null
	sudo systemctl daemon-reload
	sudo systemctl enable --now cxlporter-paper.service
	for i in \$(seq 1 30); do
		ss -lnt 'sport = :7100' | grep -q LISTEN && break
		sleep 1
	done
	ss -lnt 'sport = :7100' | grep LISTEN
	/usr/local/bin/cxlporter-paper client 127.0.0.1 7100 PING 1
	/usr/local/bin/cxlporter-paper client 127.0.0.1 7100 POOL_STATUS 2
	pid=\$(systemctl show -p MainPID --value cxlporter-paper.service)
	if sudo find \"/proc/\$pid/fd\" -maxdepth 1 -type l -lname '*cxlfork*' |
	   grep -q .; then
		echo 'CXLP/2 daemon unexpectedly holds /dev/cxlfork' >&2
		exit 1
	fi
	sudo docker logs --tail 20 '$CONTAINER'
	sudo docker inspect --format \
		'ghost_container id={{.Id}} image={{.Config.Image}} pid={{.State.Pid}} running={{.State.Running}}' \
		'$CONTAINER'
"

"$PORTER/cxlporter-paper" client \
	"$CONTROL_HOST" "$CONTROL_PORT" PING 3
echo "paper_ghost_deployed image=$IMAGE container=$CONTAINER verify=ok"
