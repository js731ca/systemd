/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "ask-password-api.h"
#include "cryptenroll-fido2.h"
#include "cryptsetup-fido2.h"
#include "hexdecoct.h"
#include "json-util.h"
#include "libfido2-util.h"
#include "memory-util.h"
#include "random-util.h"

int load_volume_key_fido2(
                struct crypt_device *cd,
                const char *cd_node,
                const char *device,
                void *ret_vk,
                size_t *ret_vks) {

        _cleanup_(erase_and_freep) void *decrypted_key = NULL;
        _cleanup_(erase_and_freep) char *passphrase = NULL;
        size_t decrypted_key_size;
        ssize_t passphrase_size;
        int r;

        assert_se(cd);
        assert_se(cd_node);
        assert_se(ret_vk);
        assert_se(ret_vks);

        r = acquire_fido2_key_auto(
                        cd,
                        cd_node,
                        cd_node,
                        device,
                        /* until= */ 0,
                        "cryptenroll.fido2-pin",
                        ASK_PASSWORD_PUSH_CACHE|ASK_PASSWORD_ACCEPT_CACHED,
                        &decrypted_key,
                        &decrypted_key_size);
        if (r == -EAGAIN)
                return log_error_errno(r, "FIDO2 token does not exist, or UV is blocked. Please try again.");
        if (r < 0)
                return r;

        /* Because cryptenroll requires a LUKS header, we can assume that this device is not
         * a PLAIN device. In this case, we need to base64 encode the secret to use as the passphrase */
        passphrase_size = base64mem(decrypted_key, decrypted_key_size, &passphrase);
        if (passphrase_size < 0)
                return log_oom();

        r = crypt_volume_key_get(
                        cd,
                        CRYPT_ANY_SLOT,
                        ret_vk,
                        ret_vks,
                        passphrase,
                        passphrase_size);
        if (r < 0)
                return log_error_errno(r, "Unlocking via FIDO2 device failed: %m");

        return r;
}

int enroll_fido2(
                struct crypt_device *cd,
                const void *volume_key,
                size_t volume_key_size,
                const char *device,
                Fido2EnrollFlags lock_with,
                int cred_alg) {

        _cleanup_(erase_and_freep) void *salt = NULL, *secret = NULL;
        _cleanup_(erase_and_freep) char *base64_encoded = NULL;
        _cleanup_(sd_json_variant_unrefp) sd_json_variant *v = NULL;
        _cleanup_free_ char *keyslot_as_string = NULL;
        size_t cid_size, salt_size, secret_size;
        _cleanup_free_ void *cid = NULL;
        ssize_t base64_encoded_size;
        const char *node, *un;
        int r, keyslot;

        assert_se(cd);
        assert_se(volume_key);
        assert_se(volume_key_size > 0);
        assert_se(device);

        assert_se(node = crypt_get_device_name(cd));

        un = strempty(crypt_get_uuid(cd));

        r = fido2_generate_hmac_hash(
                        device,
                        /* rp_id= */ "io.systemd.cryptsetup",
                        /* rp_name= */ "Encrypted Volume",
                        /* user_id= */ un, strlen(un), /* We pass the user ID and name as the same: the disk's UUID if we have it */
                        /* user_name= */ un,
                        /* user_display_name= */ node,
                        /* user_icon_name= */ NULL,
                        /* askpw_icon_name= */ "drive-harddisk",
                        /* askpw_credential= */ "cryptenroll.fido2-pin",
                        lock_with,
                        cred_alg,
                        &cid, &cid_size,
                        &salt, &salt_size,
                        &secret, &secret_size,
                        NULL,
                        &lock_with);
        if (r < 0)
                return r;

        /* Before we use the secret, we base64 encode it, for compat with homed, and to make it easier to type in manually */
        base64_encoded_size = base64mem(secret, secret_size, &base64_encoded);
        if (base64_encoded_size < 0)
                return log_error_errno(base64_encoded_size, "Failed to base64 encode secret key: %m");

        r = cryptsetup_set_minimal_pbkdf(cd);
        if (r < 0)
                return log_error_errno(r, "Failed to set minimal PBKDF: %m");

        keyslot = crypt_keyslot_add_by_volume_key(
                        cd,
                        CRYPT_ANY_SLOT,
                        volume_key,
                        volume_key_size,
                        base64_encoded,
                        base64_encoded_size);
        if (keyslot < 0)
                return log_error_errno(keyslot, "Failed to add new FIDO2 key to %s: %m", node);

        if (asprintf(&keyslot_as_string, "%i", keyslot) < 0)
                return log_oom();

        r = sd_json_buildo(&v,
                           SD_JSON_BUILD_PAIR("type", JSON_BUILD_CONST_STRING("systemd-fido2")),
                           SD_JSON_BUILD_PAIR("keyslots", SD_JSON_BUILD_ARRAY(SD_JSON_BUILD_STRING(keyslot_as_string))),
                           SD_JSON_BUILD_PAIR("fido2-credential", SD_JSON_BUILD_BASE64(cid, cid_size)),
                           SD_JSON_BUILD_PAIR("fido2-salt", SD_JSON_BUILD_BASE64(salt, salt_size)),
                           SD_JSON_BUILD_PAIR("fido2-rp", JSON_BUILD_CONST_STRING("io.systemd.cryptsetup")),
                           SD_JSON_BUILD_PAIR("fido2-clientPin-required", SD_JSON_BUILD_BOOLEAN(FLAGS_SET(lock_with, FIDO2ENROLL_PIN))),
                           SD_JSON_BUILD_PAIR("fido2-up-required", SD_JSON_BUILD_BOOLEAN(FLAGS_SET(lock_with, FIDO2ENROLL_UP))),
                           SD_JSON_BUILD_PAIR("fido2-uv-required", SD_JSON_BUILD_BOOLEAN(FLAGS_SET(lock_with, FIDO2ENROLL_UV))));
        if (r < 0)
                return log_error_errno(r, "Failed to prepare FIDO2 JSON token object: %m");

        r = cryptsetup_add_token_json(cd, v);
        if (r < 0)
                return log_error_errno(r, "Failed to add FIDO2 JSON token to LUKS2 header: %m");

        log_info("New FIDO2 token enrolled as key slot %i.", keyslot);
        return keyslot;
}
