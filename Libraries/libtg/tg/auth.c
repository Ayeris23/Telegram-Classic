#include "../config.h"
#include "tg.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strtok_foreach.h"
#include "../tl/serialize.h"
#include "../mtx/include/types.h"
#include "../mtx/include/api.h"
#include <sys/socket.h>
#include "errors.h"

#define AUTH_TRACE(...)             \
    do {                            \
        printf(__VA_ARGS__);        \
        fflush(stdout);             \
    } while (0)

buf_t initConnection(tg_t *tg, buf_t query)
{
    buf_t initConnection =
        tl_initConnection(
            tg->apiId,
            PACKAGE_NAME,
            PACKAGE_VERSION,
            PACKAGE_VERSION,
            "ru",
            "LibTg",
            "ru",
            NULL,
            NULL,
            &query);

    /*ON_LOG_BUF(tg, initConnection, */
    /*"%s: initConnection: ", __func__);*/

    buf_t invokeWithLayer =
        tl_invokeWithLayer(
            API_LAYER, &initConnection);

    /*ON_LOG_BUF(tg, invokeWithLayer, */
    /*"%s: invokeWithLayer: ", __func__);*/

    return invokeWithLayer;
}

tl_user_t *
tg_is_authorized(tg_t *tg)
{
    if (tg->key.size) {
        ON_LOG(tg, "have auth_key with len: %d", tg->key.size);

        // init connection and get config
        buf_t getConfig = tl_help_getConfig();
        buf_t init = initConnection(tg, getConfig);
        buf_free(getConfig);

        tl_t *tl = tg_send_query(tg, &init);
        buf_free(init);

        if (tl == NULL || tl->_id != id_config) {
            ON_ERR(tg, "can't get config!");
            return NULL;
        }

        tg->config = (tl_config_t *)tl;
        tl = NULL;

        // check if authorized
        InputUser iuser = tl_inputUserSelf();
        /*ON_LOG_BUF(tg, iuser, "%s: InputUser: ", __func__);*/

        buf_t getUsers =
            tl_users_getUsers(&iuser, 1);
        /*ON_LOG_BUF(tg, getUsers, "%s: getUsers: ", __func__);*/
        buf_free(iuser);

        tl = tg_send_query(tg, &getUsers);
        buf_free(getUsers);

        if (tl == NULL) {
            return NULL;
        }

        if (tl->_id == id_vector) {
            tl_vector_t *vector = (tl_vector_t *)tl;
            ON_LOG(tg, "got vector with len: %d", vector->len_);
            /*ON_LOG_BUF(tg, vector->data_, "VECTOR DATA: ");*/

            tl_t *user = tl_deserialize(&vector->data_);
            if (user && user->_id == id_user) {
                return (tl_user_t *)user;
            }
        }

        return NULL;
    }

    ON_ERR(tg, "NEED_TO_AUTHORIZE");
    return NULL;
}

tl_auth_sentCode_t *
tg_auth_sendCode(tg_t *tg, const char *phone_number)
{
    tl_t *tl = NULL;

    AUTH_TRACE("AUTH: entered tg_auth_sendCode, phone=%s\n",
               phone_number ? phone_number : "(null)");

    ON_LOG(tg, "%s", __func__);

    // init connection and get config
    buf_t getConfig = tl_help_getConfig();
    buf_t init = initConnection(tg, getConfig);
    buf_free(getConfig);

    AUTH_TRACE("AUTH: before help.getConfig query\n");

    tl = tg_send_query(tg, &init);
    buf_free(init);

    AUTH_TRACE("AUTH: after help.getConfig query, tl=%p\n",
               (void *)tl);

    if (tl != NULL) {
        AUTH_TRACE("AUTH: help.getConfig response id=0x%08x\n",
                   (unsigned int)tl->_id);

        if (tl->_id == id_rpc_error) {
            AUTH_TRACE("AUTH: help.getConfig RPC error: %s\n",
                       RPC_ERROR(tl));
        }
    }

    if (tl == NULL || tl->_id != id_config) {
        AUTH_TRACE("AUTH: help.getConfig failed, tl=%p\n",
                   (void *)tl);

        ON_ERR(tg, "can't get config!");

        if (tl != NULL) {
            tl_free(tl);
        }

        return NULL;
    }

    AUTH_TRACE("AUTH: help.getConfig accepted\n");

    tg->config = (tl_config_t *)tl;

    // get tokens from database
    buf_t t[20];
    int tn = 0;
    char *auth_tokens = auth_tokens_from_database(tg);

    if (auth_tokens) {
        strtok_foreach(auth_tokens, ";", token) {
            if (tn >= 20) {
                AUTH_TRACE("AUTH: token list truncated at 20 entries\n");
                break;
            }

            t[tn++] =
                buf_add((uint8_t *)token, strlen(token));
        }
    }

    CodeSettings codeSettings =
        tl_codeSettings(
            false,
            false,
            false,
            false,
            false,
            false,
            auth_tokens ? t : NULL,
            tn,
            NULL,
            NULL);

    /*ON_LOG_BUF(tg, codeSettings, */
    /*"%s: codeSettings: ", __func__);*/

    AUTH_TRACE("AUTH: stored apiId=%d\n", tg->apiId);
    
    AUTH_TRACE("AUTH: stored apiHash=%p, length=%lu\n",
               (void *)tg->apiHash,
               tg->apiHash
               ? (unsigned long)strlen(tg->apiHash)
               : 0UL);
    buf_t sendCode =
        tl_auth_sendCode(
            phone_number,
            tg->apiId,
            tg->apiHash,
            &codeSettings);

    /*ON_LOG_BUF(tg, sendCode, */
    /*"%s: sendCode: ", __func__);*/

    buf_free(codeSettings);

    AUTH_TRACE("AUTH: before auth.sendCode query, phone=%s\n",
               phone_number ? phone_number : "(null)");

    tl = tg_send_query(tg, &sendCode);
    buf_free(sendCode);

    AUTH_TRACE("AUTH: after auth.sendCode query, tl=%p\n",
               (void *)tl);

    if (tl == NULL) {
        AUTH_TRACE("AUTH: auth.sendCode returned NULL\n");
        return NULL;
    }

    AUTH_TRACE("AUTH: auth.sendCode response id=0x%08x\n",
               (unsigned int)tl->_id);

    if (tl->_id == id_rpc_error) {
        tl_rpc_error_t *error = (tl_rpc_error_t *)tl;

        AUTH_TRACE("AUTH: auth.sendCode RPC error: %s\n",
                   RPC_ERROR(tl));

        char *str =
            strstr((char *)error->error_message_.data,
                   "PHONE_MIGRATE_");

        if (str) {
            str += strlen("PHONE_MIGRATE_");

            int dc = atoi(str);
            const char *ip =
                tg_ip_address_for_dc(tg, dc);

            AUTH_TRACE("AUTH: migrating to DC %d, ip=%s\n",
                       dc,
                       ip ? ip : "(null)");

            if (!ip) {
                tl_free(tl);
                return NULL;
            }

            tg_set_server_address(tg, ip, 443);
            
            /*
             * The query worker has already closed the socket before
             * tg_send_query() returned. Do not close shared_rc.net again.
             * It still contains the old descriptor number, so invalidate it.
             */
            shared_rc.net.sockfd = -1;
            
            /*
             * A different data center needs a fresh authorization key and
             * MTProto session state.
             */
            if (tg->key.size > 0) {
                buf_free(tg->key);
                memset(&tg->key, 0, sizeof(tg->key));
            }
            
            if (tg->salt.size > 0) {
                buf_free(tg->salt);
                memset(&tg->salt, 0, sizeof(tg->salt));
            }
            
            if (tg->ssid.size > 0) {
                buf_free(tg->ssid);
                memset(&tg->ssid, 0, sizeof(tg->ssid));
            }
            
            tg->key_id = 0;
            tg->seqn = 0;
            
            tl_free(tl);
            
            AUTH_TRACE("AUTH: retrying auth.sendCode on migrated DC\n");
            
            return tg_auth_sendCode(tg, phone_number);
        }

        tl_free(tl);
        return NULL;
    }

    if (tl->_id == id_auth_sentCode) {
        AUTH_TRACE("AUTH: received auth.sentCode successfully\n");
        return (tl_auth_sentCode_t *)tl;
    }

    AUTH_TRACE("AUTH: unexpected response id=0x%08x\n",
               (unsigned int)tl->_id);

    tl_free(tl);
    return NULL;
}

tl_user_t *
tg_auth_signIn(tg_t *tg,
               tl_auth_sentCode_t *sentCode,
               const char *phone_number,
               const char *phone_code)
{
    ON_LOG(tg, "%s", __func__);

    buf_t signIn =
        tl_auth_signIn(
            phone_number,
            (char *)sentCode->phone_code_hash_.data,
            phone_code,
            NULL);

    tl_t *tl =
        tg_send_query(tg, &signIn);

    buf_free(signIn);

    if (tl && tl->_id == id_auth_authorization) {
        tl_auth_authorization_t *auth =
            (tl_auth_authorization_t *)tl;

//        if (auth->setup_password_required_) {
//            // throw error
//            ON_ERR(tg, "SESSION_PASSWORD_NEEDED");
//            return NULL;
//        }

        if (auth->future_auth_token_.size > 0) {
            // save auth token
            char auth_token[BUFSIZ];
            strncpy(
                auth_token,
                (char *)auth->future_auth_token_.data,
                auth->future_auth_token_.size);

            auth_token_to_database(tg, auth_token);
        }

        // save auth_key_id
        auth_key_to_database(tg, tg->key);

        // save ip address
        ip_address_to_database(tg, tg->ip);

        return (tl_user_t *)auth->user_;
    }

    if (tl) {
        tl_free(tl);
    }

    return NULL;
}
