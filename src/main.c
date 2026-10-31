/* Start Crimson Chat, an IRC client with its own bouncer
 *
 */

#include "app/application.h"

int main(int argument_count, char **arguments) {
	return run_application(&APPLICATION_SETUP, argument_count, arguments);
}
