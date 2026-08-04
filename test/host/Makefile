CORE = ../../core
CFLAGS = -I$(CORE) -Wall -Wextra -std=c11
TESTS = test_ftms_parse test_ftms_devlist test_ifit_parse test_ctrl_dispatch test_ant_sdm_encode test_ifit_fsm test_connect_policy test_connect_backoff test_ctrl_frames test_workout_ctrl

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_ftms_parse: test_ftms_parse.c $(CORE)/ftms_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ftms_devlist: test_ftms_devlist.c $(CORE)/ftms_devlist.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ifit_parse: test_ifit_parse.c $(CORE)/ifit_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ctrl_dispatch: test_ctrl_dispatch.c $(CORE)/ctrl_dispatch.c $(CORE)/ftms_devlist.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ant_sdm_encode: test_ant_sdm_encode.c $(CORE)/ant_sdm_encode.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ifit_fsm: test_ifit_fsm.c $(CORE)/ifit_fsm.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_connect_policy: test_connect_policy.c $(CORE)/connect_policy.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_connect_backoff: test_connect_backoff.c $(CORE)/connect_backoff.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ctrl_frames: test_ctrl_frames.c $(CORE)/ctrl_frames.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_workout_ctrl: test_workout_ctrl.c $(CORE)/workout_ctrl.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f $(TESTS)
