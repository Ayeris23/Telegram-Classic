#include "pbkdf2.h"
#include <openssl/evp.h>
#include <assert.h>

buf_t tg_pbkdf2_sha512(
		buf_t password, buf_t salt, int iteration_count)
{
	assert(iteration_count > 0);
	
	const EVP_MD *evp_md = EVP_sha512();
  int hash_size = EVP_MD_size(evp_md);
	
	buf_t dest = buf_new();
	dest.size = hash_size;
	
/*#if OPENSSL_VERSION_NUMBER < 0x10000000L*/
#ifdef __APPLE__
    if (PKCS5_PBKDF2_HMAC(
                          (const char *)password.data,
                          (int)password.size,
                          salt.data,
                          (int)salt.size,
                          iteration_count,
                          evp_md,
                          hash_size,
                          dest.data) != 1)
    {
        perror("Failed to PBKDF2");
        return dest;
    }
#endif
	
	dest.size = hash_size;
	return dest; 
}
