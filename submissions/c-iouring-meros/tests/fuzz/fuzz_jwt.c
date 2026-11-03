/* libFuzzer: arbitrary tokens must never crash the verifier, and a token that verifies must give sane claims. */
#include <stdint.h>
#include <stdlib.h>

#include "jwt.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static jwt_verifier verifier;
	static int ready;
	jwt_claims claims;

	if (!ready) {
		jwt_verifier_init(&verifier, STRVIEW("twelve-dollar-challenge"));
		ready = 1;
	}
	if (jwt_verify(&verifier, strview_make((const char *)data, size), 1800000000, &claims) == JWT_OK) {
		if (claims.user_id <= 0 || claims.username_len >= JWT_USERNAME_CAP)
			abort();
	}
	return 0;
}
