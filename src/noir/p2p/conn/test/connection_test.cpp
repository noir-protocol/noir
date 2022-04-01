// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include <catch2/catch_all.hpp>
#include <noir/common/hex.h>
#include <noir/common/types.h>
#include <noir/crypto/hash.h>
#include <noir/p2p/conn/secret_connection.h>

#include <fc/crypto/base64.hpp>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>

#include <sodium.h>

using namespace noir;

TEST_CASE("secret_connection: make_secret_connection", "[noir][p2p]") {
  auto priv_key_str =
    fc::base64_decode("q4BNZ9LFQw60L4UzkwkmRB2x2IPJGKwUaFXzbDTAXD5RezWnXQynrSHrYj602Dt6u6ga7T5Uc1pienw7b5JAbQ==");
  std::vector<char> loc_priv_key(priv_key_str.begin(), priv_key_str.end());
  auto c = p2p::secret_connection::make_secret_connection(loc_priv_key);

  bytes32 received_pub_key{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"};
  c->shared_eph_pub_key(received_pub_key);
}

TEST_CASE("secret_connection: openssl - key gen", "[noir][p2p]") {
  EVP_PKEY* pkey = NULL;
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
  EVP_PKEY_keygen_init(pctx);
  EVP_PKEY_keygen(pctx, &pkey);
  EVP_PKEY_CTX_free(pctx);
  PEM_write_PrivateKey(stdout, pkey, NULL, NULL, 0, NULL, NULL);
}

std::string crypto_box_recover_public_key(uint8_t secret_key[]) {
  uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
  crypto_scalarmult_curve25519_base(public_key, secret_key);
  auto enc_pub_key = fc::base64_encode(public_key, crypto_box_PUBLICKEYBYTES);
  // std::cout << "recovered pub_key = " << enc_pub_key << std::endl;
  return enc_pub_key;
}

std::string crypto_sign_recover_public_key(uint8_t secret_key[]) {
  uint8_t public_key[crypto_sign_PUBLICKEYBYTES];
  memcpy(public_key, secret_key + crypto_sign_PUBLICKEYBYTES, crypto_sign_PUBLICKEYBYTES);
  auto enc_pub_key = fc::base64_encode(public_key, crypto_box_PUBLICKEYBYTES);
  // std::cout << "recovered pub_key = " << enc_pub_key << std::endl;
  return enc_pub_key;
}

TEST_CASE("secret_connection: libsodium - key gen : crypto_box", "[noir][p2p]") {
  uint8_t public_key[crypto_box_PUBLICKEYBYTES];
  uint8_t secret_key[crypto_box_SECRETKEYBYTES];
  crypto_box_keypair(public_key, secret_key);

  // std::cout << "pub_key = " << fc::base64_encode(public_key, crypto_box_PUBLICKEYBYTES) << std::endl;
  // std::cout << "pri_key = " << fc::base64_encode(secret_key, crypto_box_SECRETKEYBYTES) << std::endl;
  auto rec_pub_key = crypto_box_recover_public_key(secret_key);
  CHECK(rec_pub_key == fc::base64_encode(public_key, crypto_box_PUBLICKEYBYTES));
}

TEST_CASE("secret_connection: libsodium - key gen : crypto_sign", "[noir][p2p]") {
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  crypto_sign_keypair(pk, sk);

  // std::cout << "pub_key = " << fc::base64_encode(pk, crypto_sign_PUBLICKEYBYTES) << std::endl;
  // std::cout << "pri_key = " << fc::base64_encode(sk, crypto_sign_SECRETKEYBYTES) << std::endl;
  auto rec_pub_key = crypto_sign_recover_public_key(sk);
  CHECK(rec_pub_key == fc::base64_encode(pk, crypto_sign_PUBLICKEYBYTES));
}

TEST_CASE("secret_connection: libsodium - derive pub_key from priv_key", "[noir][p2p]") {
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  auto priv_key =
    fc::base64_decode("q4BNZ9LFQw60L4UzkwkmRB2x2IPJGKwUaFXzbDTAXD5RezWnXQynrSHrYj602Dt6u6ga7T5Uc1pienw7b5JAbQ==");
  std::vector<char> raw(priv_key.begin(), priv_key.end());
  auto rec_pub_key = crypto_sign_recover_public_key(reinterpret_cast<uint8_t*>(raw.data()));
  CHECK(rec_pub_key == "UXs1p10Mp60h62I+tNg7eruoGu0+VHNaYnp8O2+SQG0=");
}

TEST_CASE("secret_connection: derive address from pub_key", "[noir][p2p]") {
  auto pub_key = fc::base64_decode("UXs1p10Mp60h62I+tNg7eruoGu0+VHNaYnp8O2+SQG0=");
  auto h = crypto::sha256()(pub_key);
  noir::bytes address = bytes(h.begin(), h.begin() + 20);
  // std::cout << to_hex(address) << std::endl;
  CHECK(to_hex(address) == "cbc837aced724b22dc0bff1821cdbdd96164d637");
}

constexpr int MAX_MSG_LEN = 64;

int sign(uint8_t sm[], const uint8_t m[], const int mlen, const uint8_t sk[]) {
  unsigned long long smlen;

  if (crypto_sign(sm, &smlen, m, mlen, sk) == 0) {
    return smlen;
  } else {
    return -1;
  }
}

int verify(uint8_t m[], const uint8_t sm[], const int smlen, const uint8_t pk[]) {
  unsigned long long mlen;

  if (crypto_sign_open(m, &mlen, sm, smlen, pk) == 0) {
    return mlen;
  } else {
    return -1;
  }
}

TEST_CASE("secret_connection: libsodium - sign ", "[noir][p2p]") {
  uint8_t sk[crypto_sign_SECRETKEYBYTES];
  uint8_t pk[crypto_sign_PUBLICKEYBYTES];
  uint8_t sm[MAX_MSG_LEN + crypto_sign_BYTES];
  uint8_t m[MAX_MSG_LEN + crypto_sign_BYTES + 1];

  memset(m, '\0', MAX_MSG_LEN);
  std::string msg{"Hello World"};
  strcpy((char*)m, msg.data());
  int mlen = msg.size();

  int rc = crypto_sign_keypair(pk, sk);
  CHECK(rc >= 0);
  int smlen = sign(sm, m, mlen, sk);
  CHECK(smlen >= 0);
  mlen = verify(m, sm, smlen, pk);
  CHECK(mlen >= 0);
}

TEST_CASE("secret_connection: libsodium - key exchange ", "[noir][p2p]") {
  unsigned char client_publickey[crypto_box_PUBLICKEYBYTES];
  unsigned char client_secretkey[crypto_box_SECRETKEYBYTES];
  unsigned char server_publickey[crypto_box_PUBLICKEYBYTES];
  unsigned char server_secretkey[crypto_box_SECRETKEYBYTES];
  unsigned char scalarmult_q_by_client[crypto_scalarmult_BYTES];
  unsigned char scalarmult_q_by_server[crypto_scalarmult_BYTES];
  unsigned char sharedkey_by_client[crypto_generichash_BYTES];
  unsigned char sharedkey_by_server[crypto_generichash_BYTES];
  crypto_generichash_state h;

  /* Create client's secret and public keys */
  randombytes_buf(client_secretkey, sizeof client_secretkey);
  crypto_scalarmult_base(client_publickey, client_secretkey);

  /* Create server's secret and public keys */
  randombytes_buf(server_secretkey, sizeof server_secretkey);
  crypto_scalarmult_base(server_publickey, server_secretkey);

  /* The client derives a shared key from its secret key and the server's public key */
  /* shared key = h(q ‖ client_publickey ‖ server_publickey) */
  if (crypto_scalarmult(scalarmult_q_by_client, client_secretkey, server_publickey) != 0) {
    /* Error */
  }
  crypto_generichash_init(&h, NULL, 0U, sizeof sharedkey_by_client);
  crypto_generichash_update(&h, scalarmult_q_by_client, sizeof scalarmult_q_by_client);
  crypto_generichash_update(&h, client_publickey, sizeof client_publickey);
  crypto_generichash_update(&h, server_publickey, sizeof server_publickey);
  crypto_generichash_final(&h, sharedkey_by_client, sizeof sharedkey_by_client);
}

TEST_CASE("secret_connection: openssl - hkdf ", "[noir][p2p]") {
  EVP_KDF* kdf;
  EVP_KDF_CTX* kctx;
  unsigned char key[32 + 32 + 32];
  OSSL_PARAM params[5], *p = params;

  kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
  kctx = EVP_KDF_CTX_new(kdf);
  EVP_KDF_free(kdf);

  *p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, SN_sha256, strlen(SN_sha256));

  // *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void*)"secret", (size_t)6);
  bytes32 secret{"9fe4a5a73df12dbd8659b1d9280873fe993caefec6b0ebc2686dd65027148e03"};
  *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void*)secret.data(), secret.size());

  // *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void*)"label", (size_t)5);
  *p++ =
    OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void*)"TENDERMINT_SECRET_CONNECTION_KEY_AND_CHALLENGE_GEN",
      strlen("TENDERMINT_SECRET_CONNECTION_KEY_AND_CHALLENGE_GEN"));

  // *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void*)"salt", (size_t)4);
  *p = OSSL_PARAM_construct_end();
  if (EVP_KDF_derive(kctx, key, sizeof(key), params) <= 0) {
    std::cout << "Error: "
              << "EVP_KDF_derive" << std::endl;
  }
  EVP_KDF_CTX_free(kctx);
  // std::cout << "key = " << fc::base64_encode(key, sizeof(key)) << std::endl;
  // std::cout << "key1 = " << fc::base64_encode(key, 32) << std::endl;
  // std::cout << "key2 = " << fc::base64_encode(key + 32, 32) << std::endl;
  // std::cout << "challenge = " << fc::base64_encode(key + 64, 32) << std::endl;

  // std::cout << "key1_hex = " << to_hex(std::span((const byte_type*)key, 32)) << std::endl;
  // std::cout << "key2_hex = " << to_hex(std::span((const byte_type*)(key+32), 32)) << std::endl;
  // std::cout << "challenge_hex = " << to_hex(std::span((const byte_type*)(key+64), 32)) << std::endl;
  CHECK(
    to_hex(std::span((const byte_type*)key, 32)) == "80a83ad6afcb6f8175192e41973aed31dd75e3c106f813d986d9567a4865eb2f");
  CHECK(to_hex(std::span((const byte_type*)(key + 32), 32)) ==
    "96362a04f628a0666d9866147326898bb0847b8db8680263ad19e6336d4eed9e");
  CHECK(to_hex(std::span((const byte_type*)(key + 64), 32)) ==
    "2632c3fd20f456c5383ed16aa1d56dc7875a2b0fc0d5ff053c3ada8934098c69");
}