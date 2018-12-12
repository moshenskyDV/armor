// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
#include "bernstein/c_types.h"
#include "crypto-util.h"
#include "generic-ops.hpp"
#include "hash.h"

namespace crypto {

#pragma pack(push, 1)
struct Hash : public cryptoHash {
	constexpr Hash() : cryptoHash{} {}
};

struct EllipticCurvePoint : public cryptoEllipticCurvePoint {
	constexpr EllipticCurvePoint() : cryptoEllipticCurvePoint{} {}
};
struct EllipticCurveScalar : public cryptoEllipticCurveScalar {
	constexpr EllipticCurveScalar() : cryptoEllipticCurveScalar{} {}
};

struct PublicKey : public EllipticCurvePoint {};

struct SecretKey : public EllipticCurveScalar {
	~SecretKey() { sodium_memzero(data, sizeof(data)); }
};

struct KeyDerivation : public EllipticCurvePoint {};

struct KeyImage : public EllipticCurvePoint {};

struct Signature {
	EllipticCurveScalar c, r;
};
#pragma pack(pop)

static_assert(sizeof(EllipticCurvePoint) == 32 && sizeof(EllipticCurveScalar) == 32, "Invalid structure size");

static_assert(sizeof(Hash) == 32 && sizeof(PublicKey) == 32 && sizeof(SecretKey) == 32 && sizeof(KeyDerivation) == 32 &&
                  sizeof(KeyImage) == 32 && sizeof(Signature) == 64,
    "Invalid structure size");

// Following structures never used as a pod

struct CMBranchElement {
	uint8_t depth = 0;
	Hash hash;
};

struct KeyPair {
	PublicKey public_key;
	SecretKey secret_key;
};

typedef std::vector<Signature> RingSignature;

struct RingSignature2 {                  // New half-size signatures
	EllipticCurveScalar c0;              // c[0]
	std::vector<EllipticCurveScalar> r;  // r[0]..r[n]
};

struct RingSignature3 {                               // New half-size signatures
	EllipticCurveScalar c0;                           // c0
	std::vector<std::vector<EllipticCurveScalar>> r;  // r[i, j]
};

std::ostream &operator<<(std::ostream &out, const EllipticCurvePoint &v);
std::ostream &operator<<(std::ostream &out, const EllipticCurveScalar &v);
std::ostream &operator<<(std::ostream &out, const Hash &v);

CRYPTO_MAKE_COMPARABLE(Hash, std::memcmp)
CRYPTO_MAKE_COMPARABLE(EllipticCurveScalar, sodium_compare)
CRYPTO_MAKE_COMPARABLE(EllipticCurvePoint, std::memcmp)
CRYPTO_MAKE_COMPARABLE(PublicKey, std::memcmp)
CRYPTO_MAKE_COMPARABLE(SecretKey, sodium_compare)
CRYPTO_MAKE_COMPARABLE(KeyDerivation, std::memcmp)
CRYPTO_MAKE_COMPARABLE(KeyImage, std::memcmp)
CRYPTO_MAKE_COMPARABLE(Signature, std::memcmp)

}  // namespace crypto

CRYPTO_MAKE_HASHABLE(crypto::Hash)
CRYPTO_MAKE_HASHABLE(crypto::EllipticCurveScalar)
CRYPTO_MAKE_HASHABLE(crypto::EllipticCurvePoint)
CRYPTO_MAKE_HASHABLE(crypto::PublicKey)
CRYPTO_MAKE_HASHABLE(crypto::SecretKey)
CRYPTO_MAKE_HASHABLE(crypto::KeyDerivation)
CRYPTO_MAKE_HASHABLE(crypto::KeyImage)
