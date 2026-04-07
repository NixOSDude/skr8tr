/*
 * skrtrkey.c — Skr8tr Key Management Tool
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 3 — Manifest Shard (operator tool)
 *
 * Subcommands:
 *   skrtrkey keygen
 *       Generate an ML-DSA-65 keypair.
 *       Writes public key  → ./skrtrview.pub
 *       Writes secret key  → ~/.skr8tr/signing.sec  (chmod 600)
 *       Run once per installation. Distribute skrtrview.pub to conductor hosts.
 *
 *   skrtrkey sign <seckey_path> <payload>
 *       Sign <payload> with the secret key at <seckey_path>.
 *       Prints the full signed string to stdout: <payload>|<hex_sig>
 *       Used for scripting; the CLI handles signing automatically via --key.
 *
 *   skrtrkey verify <pubkey_path> <signed_cmd>
 *       Verify a signed command offline.
 *       Exits 0 on success, 1 on failure.
 */

#include "../core/skrauth.h"

#include <errno.h>
#include <oqs/oqs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * keygen
 * ---------------------------------------------------------------------- */

static int cmd_keygen(void) {
    printf("[skrtrkey] generating ML-DSA-65 keypair...\n");

    OQS_SIG *alg = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!alg) {
        fprintf(stderr, "[skrtrkey] FATAL: OQS_SIG_new failed — liboqs not available\n");
        return 1;
    }

    uint8_t *pk = malloc(alg->length_public_key);
    uint8_t *sk = malloc(alg->length_secret_key);
    if (!pk || !sk) {
        fprintf(stderr, "[skrtrkey] FATAL: out of memory\n");
        free(pk); free(sk); OQS_SIG_free(alg);
        return 1;
    }

    if (OQS_SIG_keypair(alg, pk, sk) != OQS_SUCCESS) {
        fprintf(stderr, "[skrtrkey] FATAL: keypair generation failed\n");
        free(pk); free(sk); OQS_SIG_free(alg);
        return 1;
    }

    size_t pk_len = alg->length_public_key;
    size_t sk_len = alg->length_secret_key;
    OQS_SIG_free(alg);

    /* --- Write public key → ./skrtrview.pub --- */
    const char *pub_path = SKRAUTH_PUBKEY_FILENAME;
    FILE *f = fopen(pub_path, "wb");
    if (!f) {
        fprintf(stderr, "[skrtrkey] cannot write '%s': %s\n",
                pub_path, strerror(errno));
        free(pk); memset(sk, 0, sk_len); free(sk);
        return 1;
    }
    fwrite(pk, 1, pk_len, f);
    fclose(f);
    free(pk);
    printf("[skrtrkey] public key  → %s  (%zu bytes)\n", pub_path, pk_len);

    /* --- Write secret key → ~/.skr8tr/signing.sec --- */
    const char *home = getenv("HOME");
    if (!home) home = ".";

    char dir_path[512], sec_path[576];
    snprintf(dir_path, sizeof(dir_path), "%s/.skr8tr", home);
    snprintf(sec_path, sizeof(sec_path), "%s/signing.sec", dir_path);

    /* Create ~/.skr8tr with permissions 700 */
    if (mkdir(dir_path, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "[skrtrkey] cannot create '%s': %s\n",
                dir_path, strerror(errno));
        memset(sk, 0, sk_len); free(sk);
        return 1;
    }

    f = fopen(sec_path, "wb");
    if (!f) {
        fprintf(stderr, "[skrtrkey] cannot write '%s': %s\n",
                sec_path, strerror(errno));
        memset(sk, 0, sk_len); free(sk);
        return 1;
    }
    fwrite(sk, 1, sk_len, f);
    fclose(f);
    chmod(sec_path, 0600);   /* owner read/write only */

    memset(sk, 0, sk_len);   /* scrub from heap before free */
    free(sk);

    printf("[skrtrkey] secret key  → %s  (%zu bytes, chmod 600)\n",
           sec_path, sk_len);
    printf("[skrtrkey] keypair ready.\n");
    printf("\n");
    printf("  Next steps:\n");
    printf("  1. Copy skrtrview.pub alongside bin/skr8tr_sched on every host\n");
    printf("     that runs the Conductor.\n");
    printf("  2. Start the Conductor:  bin/skr8tr_sched\n");
    printf("     (auto-detects skrtrview.pub in working directory)\n");
    printf("  3. Use the CLI with your key:\n");
    printf("     skr8tr --key %s up app.skr8tr\n", sec_path);
    return 0;
}

/* -------------------------------------------------------------------------
 * sign — sign a payload string, print signed command to stdout
 * ---------------------------------------------------------------------- */

static int cmd_sign(const char *seckey_path, const char *payload) {
    char out[16384 + SKRAUTH_HEXSIG_LEN + 32];
    char err[256];

    if (skrauth_sign(payload, seckey_path, out, sizeof(out), err, sizeof(err)) < 0) {
        fprintf(stderr, "[skrtrkey] sign failed: %s\n", err);
        return 1;
    }
    printf("%s\n", out);
    return 0;
}

/* -------------------------------------------------------------------------
 * verify — verify a signed command offline
 * ---------------------------------------------------------------------- */

static int cmd_verify(const char *pubkey_path, const char *signed_cmd) {
    char bare[16384];
    if (skrauth_verify(signed_cmd, pubkey_path, bare, sizeof(bare)) == 0) {
        printf("[skrtrkey] VALID\n");
        printf("  bare command: %s\n", bare);
        return 0;
    }
    fprintf(stderr, "[skrtrkey] INVALID — bad signature, expired nonce, or wrong key\n");
    return 1;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  skrtrkey keygen\n"
        "      Generate ML-DSA-65 keypair.\n"
        "      Public key  → ./skrtrview.pub\n"
        "      Secret key  → ~/.skr8tr/signing.sec  (chmod 600)\n"
        "\n"
        "  skrtrkey sign <seckey_path> <payload>\n"
        "      Sign payload. Prints signed command to stdout.\n"
        "\n"
        "  skrtrkey verify <pubkey_path> <signed_cmd>\n"
        "      Verify signed command offline. Exits 0=valid, 1=invalid.\n"
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(); return 1; }

    if (!strcmp(argv[1], "keygen")) {
        return cmd_keygen();
    }

    if (!strcmp(argv[1], "sign")) {
        if (argc < 4) {
            fprintf(stderr, "usage: skrtrkey sign <seckey_path> <payload>\n");
            return 1;
        }
        return cmd_sign(argv[2], argv[3]);
    }

    if (!strcmp(argv[1], "verify")) {
        if (argc < 4) {
            fprintf(stderr, "usage: skrtrkey verify <pubkey_path> <signed_cmd>\n");
            return 1;
        }
        return cmd_verify(argv[2], argv[3]);
    }

    fprintf(stderr, "unknown subcommand: %s\n\n", argv[1]);
    usage();
    return 1;
}
