/*
 * SSoA LEVEL 3: MANIFEST SHARD
 * FILE: src/cockpit/gen_skrtrpass.c
 * MISSION: SkrtrPass token minting tool.
 *          Generates ML-DSA-65 keypairs and signs operator/admin tokens.
 *
 * USAGE:
 *   gen_skrtrpass keygen
 *       Generate a fresh ML-DSA-65 keypair.
 *       Writes: skrtrview.pub  (1952 bytes, raw binary — deploy to cockpit host)
 *               skrtrview.sec  (4032 bytes, raw binary — KEEP SECRET)
 *
 *   gen_skrtrpass mint --role operator --user captain --ttl 2592000 --key skrtrview.sec
 *       Mint a token for 'captain' as operator, valid 30 days.
 *       Prints the token to stdout — pipe or copy to the browser.
 *
 *   gen_skrtrpass verify --token <token> --pubkey skrtrview.pub
 *       Verify a token offline. Exits 0 on valid, 1 on invalid.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "skrtrpass.h"

/* -------------------------------------------------------------------------
 * keygen subcommand
 * ---------------------------------------------------------------------- */

static int cmd_keygen(void) {
    uint8_t pubkey[SKRTRPASS_PUBKEY_LEN];
    uint8_t seckey[SKRTRPASS_SECKEY_LEN];

    printf("[gen_skrtrpass] Generating ML-DSA-65 keypair…\n");

    OQS_STATUS rc = OQS_SIG_ml_dsa_65_keypair(pubkey, seckey);
    if (rc != OQS_SUCCESS) {
        fprintf(stderr, "[gen_skrtrpass] FATAL: keypair generation failed\n");
        return 1;
    }

    /* Write public key */
    FILE* fpub = fopen("skrtrview.pub", "wb");
    if (!fpub) { perror("fopen skrtrview.pub"); return 1; }
    if (fwrite(pubkey, 1, SKRTRPASS_PUBKEY_LEN, fpub) != SKRTRPASS_PUBKEY_LEN) {
        fprintf(stderr, "[gen_skrtrpass] FATAL: write pubkey failed\n");
        fclose(fpub);
        return 1;
    }
    fclose(fpub);

    /* Write secret key */
    FILE* fsec = fopen("skrtrview.sec", "wb");
    if (!fsec) { perror("fopen skrtrview.sec"); return 1; }
    if (fwrite(seckey, 1, SKRTRPASS_SECKEY_LEN, fsec) != SKRTRPASS_SECKEY_LEN) {
        fprintf(stderr, "[gen_skrtrpass] FATAL: write seckey failed\n");
        fclose(fsec);
        return 1;
    }
    fclose(fsec);

    printf("[gen_skrtrpass] Public key:  skrtrview.pub  (%d bytes)\n",
           SKRTRPASS_PUBKEY_LEN);
    printf("[gen_skrtrpass] Secret key:  skrtrview.sec  (%d bytes)  ← KEEP SECRET\n",
           SKRTRPASS_SECKEY_LEN);
    printf("[gen_skrtrpass] Deploy skrtrview.pub alongside skr8tr_cockpit.\n");
    printf("[gen_skrtrpass] Use 'mint' to issue tokens.\n");

    /* Zero secret key from memory */
    memset(seckey, 0, SKRTRPASS_SECKEY_LEN);
    return 0;
}

/* -------------------------------------------------------------------------
 * mint subcommand
 * ---------------------------------------------------------------------- */

static int cmd_mint(int argc, char* argv[]) {
    const char* role    = "operator";
    const char* user    = "captain";
    const char* keypath = "skrtrview.sec";
    uint64_t    ttl     = 2592000;  /* 30 days */

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--role") && i + 1 < argc)
            role = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)
            user = argv[++i];
        else if (!strcmp(argv[i], "--ttl") && i + 1 < argc)
            ttl = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc)
            keypath = argv[++i];
    }

    if (strcmp(role, "operator") && strcmp(role, "admin")) {
        fprintf(stderr, "[gen_skrtrpass] ERROR: role must be 'operator' or 'admin'\n");
        return 1;
    }

    char token[SKRTRPASS_TOKEN_MAX];
    int rc = skrtrpass_mint(role, user, ttl, keypath, token, sizeof(token));
    if (rc < 0) {
        fprintf(stderr, "[gen_skrtrpass] ERROR: mint failed (code %d)\n", rc);
        if (rc == SKRTRPASS_ERR_PUBKEY)
            fprintf(stderr, "  Cannot open secret key: %s\n", keypath);
        return 1;
    }

    printf("%s\n", token);

    uint64_t expiry = (uint64_t)time(NULL) + ttl;
    fprintf(stderr, "[gen_skrtrpass] Token minted: role=%s user=%s expires=%llu\n",
            role, user, (unsigned long long)expiry);
    return 0;
}

/* -------------------------------------------------------------------------
 * verify subcommand
 * ---------------------------------------------------------------------- */

static int cmd_verify(int argc, char* argv[]) {
    const char* token   = NULL;
    const char* pubpath = "skrtrview.pub";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc)
            pubpath = argv[++i];
    }

    if (!token) {
        fprintf(stderr, "[gen_skrtrpass] ERROR: --token required\n");
        return 1;
    }

    int result = skrtrpass_verify(token, pubpath);
    switch (result) {
        case SKRTRPASS_OK_OPERATOR:
            printf("[gen_skrtrpass] VALID  — role: operator\n"); return 0;
        case SKRTRPASS_OK_ADMIN:
            printf("[gen_skrtrpass] VALID  — role: admin\n");    return 0;
        case SKRTRPASS_ERR_PARSE:
            printf("[gen_skrtrpass] INVALID — malformed token\n");    break;
        case SKRTRPASS_ERR_EXPIRED:
            printf("[gen_skrtrpass] INVALID — token expired\n");      break;
        case SKRTRPASS_ERR_PUBKEY:
            printf("[gen_skrtrpass] INVALID — cannot load pubkey: %s\n", pubpath); break;
        case SKRTRPASS_ERR_BADSIG:
            printf("[gen_skrtrpass] INVALID — signature verification failed\n");  break;
        case SKRTRPASS_ERR_ROLE:
            printf("[gen_skrtrpass] INVALID — unknown role in token\n"); break;
        default:
            printf("[gen_skrtrpass] INVALID — code %d\n", result); break;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  gen_skrtrpass keygen\n"
        "      Generate ML-DSA-65 keypair → skrtrview.pub / skrtrview.sec\n\n"
        "  gen_skrtrpass mint [--role operator|admin] [--user <name>]\n"
        "                     [--ttl <seconds>] [--key skrtrview.sec]\n"
        "      Sign and print a SkrtrPass token to stdout\n\n"
        "  gen_skrtrpass verify --token <token> [--pubkey skrtrview.pub]\n"
        "      Verify a token offline\n"
    );
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(); return 1; }

    if (!strcmp(argv[1], "keygen")) return cmd_keygen();
    if (!strcmp(argv[1], "mint"))   return cmd_mint(argc, argv);
    if (!strcmp(argv[1], "verify")) return cmd_verify(argc, argv);

    fprintf(stderr, "[gen_skrtrpass] unknown command: %s\n", argv[1]);
    usage();
    return 1;
}
