# Target library
lib := libfs.a


#objects.
objects := disk.o
objects += fs.o

#flags and variables
CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -MMD
#CFLAGS  += -g
AR      := ar
ARFLAGS := rcs


ifneq ($(V),1)
Q=@
endif


all: $(lib)

# Dependency tracking:
deps := $(patsubst %.o,%.d,$(objects))
-include $(deps)

%.a : $(objects)
	@echo "AR $@"
	$(Q)$(AR) $(ARFLAGS) $@ $^

%.o: %.c
	@echo "CC $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo "clean"
	$(Q)rm -f $(lib) $(objects) $(deps)
