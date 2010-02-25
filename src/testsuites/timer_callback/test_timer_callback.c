/**
 * @file
 * @date 28.01.2009
 * @author Alexandr Batyukov, Alexey Fomin, Eldar Abusalimov
 */

#include <common.h>
#include <kernel/timer.h>
#include <express_tests.h>
#include <kernel/init.h>

DECLARE_EXPRESS_TEST(timer_callback, exec, NULL);

volatile static bool tick_happened;

static void test_timer_handler(uint32_t id) {
	tick_happened = true;
}

static int exec(int argc, char** argv) {
	uint32_t id, ticks;
	long i;
	id = 17;
	ticks = 2;
	/* Timer value changing means ok */
	tick_happened = false;
	set_timer(id, ticks, test_timer_handler);
	for (i = 0; i < (1 << 20); i++) {
		if (tick_happened)
			break;
	}
	close_timer(id);

	return tick_happened ? 0 : -1;
}
