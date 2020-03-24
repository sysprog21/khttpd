CFILES = main.c http_server.c http_parser.c

obj-m += khttpd.o
khttpd-objs := $(CFILES:.c=.o)

all: http_parser.c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

# Download http_parser.[ch] from nodejs/http-parser repository
# the inclusion of standard header files such as <string.h> will be replaced
# with "compat/string.h", which is just a wrapper to Linux kernel headers.
# TODO: rewrite with elegant scripts
http_parser.c:
	wget -q https://raw.githubusercontent.com/nodejs/http-parser/master/http_parser.c
	@sed -i 's/#include <assert.h>/#include "compat\/assert.h"/' $@
	@sed -i 's/#include <stddef.h>/#include "compat\/stddef.h"/' $@
	@sed -i 's/#include <ctype.h>/#include "compat\/ctype.h"/' $@
	@sed -i 's/#include <string.h>/#include "compat\/string.h"/' $@
	@sed -i 's/#include <limits.h>/#include "compat\/limits.h"/' $@
	@echo "File $@ was patched."
	wget -q https://raw.githubusercontent.com/nodejs/http-parser/master/http_parser.h
	@sed -i 's/#include <stddef.h>/#include "compat\/stddef.h"/' http_parser.h
	@sed -i 's/#include <stdint.h>/#include "compat\/stdint.h"/' http_parser.h
	@echo "File http_parser.h was patched."

distclean: clean
	$(RM) http_parser.c http_parser.h
