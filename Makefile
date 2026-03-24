CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

gpu-metrics-fix: gpu_metrics_fix.c
	$(CC) $(CFLAGS) -o $@ $

install: gpu-metrics-fix
	install -Dm755 gpu-metrics-fix $(DESTDIR)$(PREFIX)/bin/gpu-metrics-fix
	install -Dm644 gpu-metrics-fix.service $(DESTDIR)/etc/systemd/system/gpu-metrics-fix.service
	mkdir -p $(DESTDIR)/var/lib/gpu-metrics-fix
	-chcon -t bin_t $(DESTDIR)$(PREFIX)/bin/gpu-metrics-fix 2>/dev/null

uninstall:
	systemctl disable --now gpu-metrics-fix 2>/dev/null || true
	rm -f $(DESTDIR)$(PREFIX)/bin/gpu-metrics-fix
	rm -f $(DESTDIR)/etc/systemd/system/gpu-metrics-fix.service
	rm -rf $(DESTDIR)/var/lib/gpu-metrics-fix
	systemctl daemon-reload

enable:
	systemctl daemon-reload
	systemctl enable --now gpu-metrics-fix

disable:
	systemctl disable --now gpu-metrics-fix

clean:
	rm -f gpu-metrics-fix

.PHONY: install uninstall enable disable clean