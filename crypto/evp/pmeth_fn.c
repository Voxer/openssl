/*
 * Copyright 2006-2016 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include "internal/cryptlib.h"
#include "internal/evp_int.h"
#include "internal/provider.h"
#include "evp_locl.h"

static EVP_SIGNATURE *evp_signature_new(OSSL_PROVIDER *prov)
{
    EVP_SIGNATURE *signature = OPENSSL_zalloc(sizeof(EVP_SIGNATURE));

    signature->lock = CRYPTO_THREAD_lock_new();
    if (signature->lock == NULL) {
        OPENSSL_free(signature);
        return NULL;
    }
    signature->prov = prov;
    ossl_provider_up_ref(prov);
    signature->refcnt = 1;

    return signature;
}

static void *evp_signature_from_dispatch(const char *name,
                                         const OSSL_DISPATCH *fns,
                                         OSSL_PROVIDER *prov,
                                         void *vkeymgmt_data)
{
    /*
     * Signature functions cannot work without a key, and key management
     * from the same provider to manage its keys.  We therefore fetch
     * a key management method using the same algorithm and properties
     * and pass that down to evp_generic_fetch to be passed on to our
     * evp_signature_from_dispatch, which will attach the key management
     * method to the newly created key exchange method as long as the
     * provider matches.
     */
    struct keymgmt_data_st *keymgmt_data = vkeymgmt_data;
    EVP_KEYMGMT *keymgmt = EVP_KEYMGMT_fetch(keymgmt_data->ctx, name,
                                             keymgmt_data->properties);
    EVP_SIGNATURE *signature = NULL;
    int ctxfncnt = 0, signfncnt = 0, verifyfncnt = 0, verifyrecfncnt = 0;
    int gparamfncnt = 0, sparamfncnt = 0;

    if (keymgmt == NULL || EVP_KEYMGMT_provider(keymgmt) != prov) {
        ERR_raise(ERR_LIB_EVP, EVP_R_NO_KEYMGMT_AVAILABLE);
        goto err;
    }

    if ((signature = evp_signature_new(prov)) == NULL
        || (signature->name = OPENSSL_strdup(name)) == NULL) {
        ERR_raise(ERR_LIB_EVP, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    signature->keymgmt = keymgmt;
    keymgmt = NULL;              /* avoid double free on failure below */

    for (; fns->function_id != 0; fns++) {
        switch (fns->function_id) {
        case OSSL_FUNC_SIGNATURE_NEWCTX:
            if (signature->newctx != NULL)
                break;
            signature->newctx = OSSL_get_OP_signature_newctx(fns);
            ctxfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_SIGN_INIT:
            if (signature->sign_init != NULL)
                break;
            signature->sign_init = OSSL_get_OP_signature_sign_init(fns);
            signfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_SIGN:
            if (signature->sign != NULL)
                break;
            signature->sign = OSSL_get_OP_signature_sign(fns);
            signfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_VERIFY_INIT:
            if (signature->verify_init != NULL)
                break;
            signature->verify_init = OSSL_get_OP_signature_verify_init(fns);
            verifyfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_VERIFY:
            if (signature->verify != NULL)
                break;
            signature->verify = OSSL_get_OP_signature_verify(fns);
            verifyfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_VERIFY_RECOVER_INIT:
            if (signature->verify_recover_init != NULL)
                break;
            signature->verify_recover_init
                = OSSL_get_OP_signature_verify_recover_init(fns);
            verifyrecfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_VERIFY_RECOVER:
            if (signature->verify_recover != NULL)
                break;
            signature->verify_recover
                = OSSL_get_OP_signature_verify_recover(fns);
            verifyrecfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_FREECTX:
            if (signature->freectx != NULL)
                break;
            signature->freectx = OSSL_get_OP_signature_freectx(fns);
            ctxfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_DUPCTX:
            if (signature->dupctx != NULL)
                break;
            signature->dupctx = OSSL_get_OP_signature_dupctx(fns);
            break;
        case OSSL_FUNC_SIGNATURE_GET_CTX_PARAMS:
            if (signature->get_ctx_params != NULL)
                break;
            signature->get_ctx_params
                = OSSL_get_OP_signature_get_ctx_params(fns);
            gparamfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_GETTABLE_CTX_PARAMS:
            if (signature->gettable_ctx_params != NULL)
                break;
            signature->gettable_ctx_params
                = OSSL_get_OP_signature_gettable_ctx_params(fns);
            gparamfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS:
            if (signature->set_ctx_params != NULL)
                break;
            signature->set_ctx_params
                = OSSL_get_OP_signature_set_ctx_params(fns);
            sparamfncnt++;
            break;
        case OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS:
            if (signature->settable_ctx_params != NULL)
                break;
            signature->settable_ctx_params
                = OSSL_get_OP_signature_settable_ctx_params(fns);
            sparamfncnt++;
            break;
        }
    }
    if (ctxfncnt != 2
        || (signfncnt != 2 && verifyfncnt != 2 && verifyrecfncnt != 2)
        || (gparamfncnt != 0 && gparamfncnt != 2)
        || (sparamfncnt != 0 && sparamfncnt != 2)) {
        /*
         * In order to be a consistent set of functions we must have at least
         * a set of context functions (newctx and freectx) as well as a pair of
         * "signature" functions: (sign_init, sign) or (verify_init verify) or
         * (verify_recover_init, verify_recover). set_ctx_params and
         * settable_ctx_params are optional, but if one of them is present then
         * the other one must also be present. The same applies to
         * get_ctx_params and gettable_ctx_params. The dupctx function is
         * optional.
         */
        ERR_raise(ERR_LIB_EVP, EVP_R_INVALID_PROVIDER_FUNCTIONS);
        goto err;
    }

    return signature;
 err:
    EVP_SIGNATURE_free(signature);
    EVP_KEYMGMT_free(keymgmt);
    return NULL;
}

void EVP_SIGNATURE_free(EVP_SIGNATURE *signature)
{
    if (signature != NULL) {
        int i;

        CRYPTO_DOWN_REF(&signature->refcnt, &i, signature->lock);
        if (i > 0)
            return;
        EVP_KEYMGMT_free(signature->keymgmt);
        ossl_provider_free(signature->prov);
        OPENSSL_free(signature->name);
        CRYPTO_THREAD_lock_free(signature->lock);
        OPENSSL_free(signature);
    }
}

int EVP_SIGNATURE_up_ref(EVP_SIGNATURE *signature)
{
    int ref = 0;

    CRYPTO_UP_REF(&signature->refcnt, &ref, signature->lock);
    return 1;
}

OSSL_PROVIDER *EVP_SIGNATURE_provider(const EVP_SIGNATURE *signature)
{
    return signature->prov;
}

EVP_SIGNATURE *EVP_SIGNATURE_fetch(OPENSSL_CTX *ctx, const char *algorithm,
                                   const char *properties)
{
    struct keymgmt_data_st keymgmt_data;

    /*
     * A signature operation cannot work without a key, so we need key
     * management from the same provider to manage its keys.
     */
    keymgmt_data.ctx = ctx;
    keymgmt_data.properties = properties;
    return evp_generic_fetch(ctx, OSSL_OP_SIGNATURE, algorithm, properties,
                             evp_signature_from_dispatch, &keymgmt_data,
                             (int (*)(void *))EVP_SIGNATURE_up_ref,
                             (void (*)(void *))EVP_SIGNATURE_free);
}

static int evp_pkey_signature_init(EVP_PKEY_CTX *ctx, EVP_SIGNATURE *signature,
                                   int operation)
{
    int ret = 0;
    void *provkey = NULL;

    if (ctx == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    evp_pkey_ctx_free_old_ops(ctx);
    ctx->operation = operation;

    if (ctx->engine != NULL)
        goto legacy;

    if (signature != NULL) {
        if (!EVP_SIGNATURE_up_ref(signature))
            goto err;
    } else {
        int nid = ctx->pkey != NULL ? ctx->pkey->type : ctx->pmeth->pkey_id;

        /*
         * TODO(3.0): Check for legacy handling. Remove this once all all
         * algorithms are moved to providers.
         */
        if (ctx->pkey != NULL) {
            switch (ctx->pkey->type) {
            case NID_dsa:
                break;
            default:
                goto legacy;
            }
            signature = EVP_SIGNATURE_fetch(NULL, OBJ_nid2sn(nid), NULL);
        } else {
            goto legacy;
        }

        if (signature == NULL) {
            EVPerr(0, EVP_R_INITIALIZATION_ERROR);
            goto err;
        }
    }

    ctx->op.sig.signature = signature;
    if (ctx->pkey != NULL) {
        provkey = evp_keymgmt_export_to_provider(ctx->pkey, signature->keymgmt);
        if (provkey == NULL) {
            EVPerr(0, EVP_R_INITIALIZATION_ERROR);
            goto err;
        }
    }
    ctx->op.sig.sigprovctx = signature->newctx(ossl_provider_ctx(signature->prov));
    if (ctx->op.sig.sigprovctx == NULL) {
        /* The provider key can stay in the cache */
        EVPerr(0, EVP_R_INITIALIZATION_ERROR);
        goto err;
    }

    switch (operation) {
    case EVP_PKEY_OP_SIGN:
        if (signature->sign_init == NULL) {
            EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
            ret = -2;
            goto err;
        }
        ret = signature->sign_init(ctx->op.sig.sigprovctx, provkey);
        break;
    case EVP_PKEY_OP_VERIFY:
        if (signature->verify_init == NULL) {
            EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
            ret = -2;
            goto err;
        }
        ret = signature->verify_init(ctx->op.sig.sigprovctx, provkey);
        break;
    case EVP_PKEY_OP_VERIFYRECOVER:
        if (signature->verify_recover_init == NULL) {
            EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
            ret = -2;
            goto err;
        }
        ret = signature->verify_recover_init(ctx->op.sig.sigprovctx, provkey);
        break;
    default:
        EVPerr(0, EVP_R_INITIALIZATION_ERROR);
        goto err;
    }

    if (ret <= 0) {
        signature->freectx(ctx->op.sig.sigprovctx);
        ctx->op.sig.sigprovctx = NULL;
        goto err;
    }
    return 1;

 legacy:
    if (ctx->pmeth == NULL
            || (operation == EVP_PKEY_OP_SIGN && ctx->pmeth->sign == NULL)
            || (operation == EVP_PKEY_OP_VERIFY && ctx->pmeth->verify == NULL)
            || (operation == EVP_PKEY_OP_VERIFYRECOVER
                && ctx->pmeth->verify_recover == NULL)) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    switch (operation) {
    case EVP_PKEY_OP_SIGN:
        if (ctx->pmeth->sign_init == NULL)
            return 1;
        ret = ctx->pmeth->sign_init(ctx);
        break;
    case EVP_PKEY_OP_VERIFY:
        if (ctx->pmeth->verify_init == NULL)
            return 1;
        ret = ctx->pmeth->verify_init(ctx);
        break;
    case EVP_PKEY_OP_VERIFYRECOVER:
        if (ctx->pmeth->verify_recover_init == NULL)
            return 1;
        ret = ctx->pmeth->verify_recover_init(ctx);
        break;
    default:
        EVPerr(0, EVP_R_INITIALIZATION_ERROR);
        goto err;
    }
    if (ret <= 0)
        goto err;
    return ret;

 err:
    ctx->operation = EVP_PKEY_OP_UNDEFINED;
    return ret;
}

int EVP_PKEY_sign_init_ex(EVP_PKEY_CTX *ctx, EVP_SIGNATURE *signature)
{
    return evp_pkey_signature_init(ctx, signature, EVP_PKEY_OP_SIGN);
}

int EVP_PKEY_sign_init(EVP_PKEY_CTX *ctx)
{
    return evp_pkey_signature_init(ctx, NULL, EVP_PKEY_OP_SIGN);
}

int EVP_PKEY_sign(EVP_PKEY_CTX *ctx,
                  unsigned char *sig, size_t *siglen,
                  const unsigned char *tbs, size_t tbslen)
{
    int ret;

    if (ctx == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    if (ctx->operation != EVP_PKEY_OP_SIGN) {
        EVPerr(0, EVP_R_OPERATON_NOT_INITIALIZED);
        return -1;
    }

    if (ctx->op.sig.sigprovctx == NULL)
        goto legacy;

    ret = ctx->op.sig.signature->sign(ctx->op.sig.sigprovctx, sig, siglen,
                                      SIZE_MAX, tbs, tbslen);

    return ret;
 legacy:
 
    if (ctx->pmeth == NULL || ctx->pmeth->sign == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    M_check_autoarg(ctx, sig, siglen, EVP_F_EVP_PKEY_SIGN)
        return ctx->pmeth->sign(ctx, sig, siglen, tbs, tbslen);
}

int EVP_PKEY_verify_init_ex(EVP_PKEY_CTX *ctx, EVP_SIGNATURE *signature)
{
    return evp_pkey_signature_init(ctx, signature, EVP_PKEY_OP_VERIFY);
}

int EVP_PKEY_verify_init(EVP_PKEY_CTX *ctx)
{
    return evp_pkey_signature_init(ctx, NULL, EVP_PKEY_OP_VERIFY);
}

int EVP_PKEY_verify(EVP_PKEY_CTX *ctx,
                    const unsigned char *sig, size_t siglen,
                    const unsigned char *tbs, size_t tbslen)
{
    int ret;

    if (ctx == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    if (ctx->operation != EVP_PKEY_OP_VERIFY) {
        EVPerr(0, EVP_R_OPERATON_NOT_INITIALIZED);
        return -1;
    }

    if (ctx->op.sig.sigprovctx == NULL)
        goto legacy;

    ret = ctx->op.sig.signature->verify(ctx->op.sig.sigprovctx, sig, siglen,
                                        tbs, tbslen);

    return ret;
 legacy:
    if (ctx->pmeth == NULL || ctx->pmeth->verify == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    return ctx->pmeth->verify(ctx, sig, siglen, tbs, tbslen);
}

int EVP_PKEY_verify_recover_init_ex(EVP_PKEY_CTX *ctx, EVP_SIGNATURE *signature)
{
    return evp_pkey_signature_init(ctx, signature, EVP_PKEY_OP_VERIFYRECOVER);
}

int EVP_PKEY_verify_recover_init(EVP_PKEY_CTX *ctx)
{
    return evp_pkey_signature_init(ctx, NULL, EVP_PKEY_OP_VERIFYRECOVER);
}

int EVP_PKEY_verify_recover(EVP_PKEY_CTX *ctx,
                            unsigned char *rout, size_t *routlen,
                            const unsigned char *sig, size_t siglen)
{
    int ret;

    if (ctx == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }

    if (ctx->operation != EVP_PKEY_OP_VERIFYRECOVER) {
        EVPerr(0, EVP_R_OPERATON_NOT_INITIALIZED);
        return -1;
    }

    if (ctx->op.sig.sigprovctx == NULL)
        goto legacy;

    ret = ctx->op.sig.signature->verify_recover(ctx->op.sig.sigprovctx, rout,
                                                routlen,
                                                (rout == NULL ? 0 : *routlen),
                                                sig, siglen);
    return ret;
 legacy:
    if (ctx->pmeth == NULL || ctx->pmeth->verify_recover == NULL) {
        EVPerr(0, EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }
    M_check_autoarg(ctx, rout, routlen, EVP_F_EVP_PKEY_VERIFY_RECOVER)
        return ctx->pmeth->verify_recover(ctx, rout, routlen, sig, siglen);
}

int EVP_PKEY_encrypt_init(EVP_PKEY_CTX *ctx)
{
    int ret;
    if (!ctx || !ctx->pmeth || !ctx->pmeth->encrypt) {
        EVPerr(EVP_F_EVP_PKEY_ENCRYPT_INIT,
               EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }
    ctx->operation = EVP_PKEY_OP_ENCRYPT;
    if (!ctx->pmeth->encrypt_init)
        return 1;
    ret = ctx->pmeth->encrypt_init(ctx);
    if (ret <= 0)
        ctx->operation = EVP_PKEY_OP_UNDEFINED;
    return ret;
}

int EVP_PKEY_encrypt(EVP_PKEY_CTX *ctx,
                     unsigned char *out, size_t *outlen,
                     const unsigned char *in, size_t inlen)
{
    if (!ctx || !ctx->pmeth || !ctx->pmeth->encrypt) {
        EVPerr(EVP_F_EVP_PKEY_ENCRYPT,
               EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }
    if (ctx->operation != EVP_PKEY_OP_ENCRYPT) {
        EVPerr(EVP_F_EVP_PKEY_ENCRYPT, EVP_R_OPERATON_NOT_INITIALIZED);
        return -1;
    }
    M_check_autoarg(ctx, out, outlen, EVP_F_EVP_PKEY_ENCRYPT)
        return ctx->pmeth->encrypt(ctx, out, outlen, in, inlen);
}

int EVP_PKEY_decrypt_init(EVP_PKEY_CTX *ctx)
{
    int ret;
    if (!ctx || !ctx->pmeth || !ctx->pmeth->decrypt) {
        EVPerr(EVP_F_EVP_PKEY_DECRYPT_INIT,
               EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }
    ctx->operation = EVP_PKEY_OP_DECRYPT;
    if (!ctx->pmeth->decrypt_init)
        return 1;
    ret = ctx->pmeth->decrypt_init(ctx);
    if (ret <= 0)
        ctx->operation = EVP_PKEY_OP_UNDEFINED;
    return ret;
}

int EVP_PKEY_decrypt(EVP_PKEY_CTX *ctx,
                     unsigned char *out, size_t *outlen,
                     const unsigned char *in, size_t inlen)
{
    if (!ctx || !ctx->pmeth || !ctx->pmeth->decrypt) {
        EVPerr(EVP_F_EVP_PKEY_DECRYPT,
               EVP_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE);
        return -2;
    }
    if (ctx->operation != EVP_PKEY_OP_DECRYPT) {
        EVPerr(EVP_F_EVP_PKEY_DECRYPT, EVP_R_OPERATON_NOT_INITIALIZED);
        return -1;
    }
    M_check_autoarg(ctx, out, outlen, EVP_F_EVP_PKEY_DECRYPT)
        return ctx->pmeth->decrypt(ctx, out, outlen, in, inlen);
}
