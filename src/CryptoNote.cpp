// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include "CryptoNote.hpp"
#include "Core/CryptoNoteTools.hpp"
#include "Core/TransactionExtra.hpp"
#include "CryptoNoteConfig.hpp"  // We access TRANSACTION_VERSION_AMETHYST directly
#include "rpc_api.hpp"
#include "seria/JsonOutputStream.hpp"
// includes below are for proof seria
#include "Core/Currency.hpp"
#include "common/Base58.hpp"
#include "common/Varint.hpp"
#include "seria/BinaryInputStream.hpp"
#include "seria/BinaryOutputStream.hpp"

using namespace cn;

namespace seria {

void ser(Hash &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }
void ser(KeyImage &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }
void ser(PublicKey &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }
void ser(SecretKey &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }
void ser(KeyDerivation &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }
void ser(Signature &v, ISeria &s) { s.binary(reinterpret_cast<uint8_t *>(&v), sizeof(Signature)); }
void ser(crypto::EllipticCurveScalar &v, ISeria &s) { s.binary(v.data, sizeof(v.data)); }

void ser_members(cn::AccountAddressSimple &v, ISeria &s) {
	seria_kv("spend", v.spend_public_key, s);
	seria_kv("view", v.view_public_key, s);
}
void ser_members(cn::AccountAddressUnlinkable &v, ISeria &s) {
	seria_kv("s", v.s, s);
	seria_kv("sv", v.sv, s);
}
void ser_members(AccountAddress &v, ISeria &s) {
	if (s.is_input()) {
		uint8_t type = 0;
		s.object_key("type");
		if (dynamic_cast<seria::JsonInputStream *>(&s)) {
			std::string str_type_tag;
			ser(str_type_tag, s);
			if (str_type_tag == AccountAddressSimple::str_type_tag())
				type = AccountAddressSimple::type_tag;
			else if (str_type_tag == AccountAddressUnlinkable::str_type_tag())
				type = AccountAddressUnlinkable::type_tag;
			else if (str_type_tag == AccountAddressUnlinkable::str_type_tag_auditable())
				type = AccountAddressUnlinkable::type_tag_auditable;
			else
				throw std::runtime_error("Deserialization error - unknown address type " + str_type_tag);
		} else
			s.binary(&type, 1);
		switch (type) {
		case AccountAddressSimple::type_tag: {
			AccountAddressSimple in{};
			ser_members(in, s);
			v = in;
			break;
		}
		case AccountAddressUnlinkable::type_tag:
		case AccountAddressUnlinkable::type_tag_auditable: {
			AccountAddressUnlinkable in{};
			in.is_auditable = (type == AccountAddressUnlinkable::type_tag_auditable);
			ser_members(in, s);
			v = in;
			break;
		}
		default:
			throw std::runtime_error("Deserialization error - unknown address type " + common::to_string(type));
		}
		return;
	}
	if (v.type() == typeid(AccountAddressSimple)) {
		auto &in = boost::get<AccountAddressSimple>(v);
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = AccountAddressSimple::str_type_tag();
			ser(str_type_tag, s);
		} else {
			uint8_t type = AccountAddressSimple::type_tag;
			s.binary(&type, 1);
		}
		ser_members(in, s);
	} else if (v.type() == typeid(AccountAddressUnlinkable)) {
		auto &in = boost::get<AccountAddressUnlinkable>(v);
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = in.is_auditable ? AccountAddressUnlinkable::str_type_tag_auditable()
			                                           : AccountAddressUnlinkable::str_type_tag();
			ser(str_type_tag, s);
		} else {
			uint8_t type =
			    in.is_auditable ? AccountAddressUnlinkable::type_tag_auditable : AccountAddressUnlinkable::type_tag;
			s.binary(&type, 1);
		}
		ser_members(in, s);
	}
}
void ser_members(cn::SendproofKey &v, ISeria &s) {
	seria_kv("derivation", v.derivation, s);
	seria_kv("signature", v.signature, s);
}
void ser_members(cn::SendproofUnlinkable::Element &v, ISeria &s) {
	seria_kv("out_index", v.out_index, s);
	seria_kv("q", v.q, s);
	seria_kv("signature", v.signature, s);
}
void ser_members(cn::SendproofUnlinkable &v, ISeria &s) { seria_kv("elements", v.elements, s); }
void ser_members(Sendproof &v, ISeria &s, const Currency &currency) {
	std::string addr;
	if (!s.is_input())
		addr = currency.account_address_as_string(v.address);
	seria_kv("address", addr, s);
	if (s.is_input() && (!currency.parse_account_address_string(addr, &v.address)))
		throw api::ErrorAddress(api::ErrorAddress::ADDRESS_FAILED_TO_PARSE, "Failed to parse wallet address", addr);
	seria_kv("transaction_hash", v.transaction_hash, s);
	seria_kv("message", v.message, s);
	seria_kv("amount", v.amount, s);
	std::string proof;
	BinaryArray binary_proof;
	if (!s.is_input()) {
		if (v.address.type() == typeid(AccountAddressSimple)) {
			auto &var    = boost::get<SendproofKey>(v.proof);
			binary_proof = seria::to_binary(var);
		} else if (v.address.type() == typeid(AccountAddressUnlinkable)) {
			auto &var    = boost::get<SendproofUnlinkable>(v.proof);
			binary_proof = seria::to_binary(var);
		} else
			throw api::ErrorAddress(api::ErrorAddress::ADDRESS_FAILED_TO_PARSE, "Unknown address type", addr);
		proof = common::base58::encode(binary_proof);
	}
	seria_kv("proof", proof, s);
	if (s.is_input()) {
		if (!common::base58::decode(proof, &binary_proof))
			throw std::runtime_error("Wrong proof format - " + proof);
		if (v.address.type() == typeid(AccountAddressSimple)) {
			SendproofKey sp;
			seria::from_binary(sp, binary_proof);
			v.proof = sp;
		} else if (v.address.type() == typeid(AccountAddressUnlinkable)) {
			SendproofUnlinkable sp;
			seria::from_binary(sp, binary_proof);
			v.proof = sp;
		} else
			throw api::ErrorAddress(api::ErrorAddress::ADDRESS_FAILED_TO_PARSE, "Unknown address type", addr);
	}
}
void ser_members(TransactionInput &v, ISeria &s) {
	if (s.is_input()) {
		uint8_t type = 0;
		s.object_key("type");
		if (dynamic_cast<seria::JsonInputStream *>(&s)) {
			std::string str_type_tag;
			ser(str_type_tag, s);
			if (str_type_tag == InputCoinbase::str_type_tag())
				type = InputCoinbase::type_tag;
			else if (str_type_tag == InputKey::str_type_tag())
				type = InputKey::type_tag;
			else
				throw std::runtime_error("Deserialization error - unknown input type " + str_type_tag);
		} else
			s.binary(&type, 1);
		switch (type) {
		case InputCoinbase::type_tag: {
			InputCoinbase in{};
			ser_members(in, s);
			v = in;
			break;
		}
		case InputKey::type_tag: {
			InputKey in{};
			ser_members(in, s);
			v = in;
			break;
		}
		default:
			throw std::runtime_error("Deserialization error - unknown input type " + common::to_string(type));
		}
		return;
	}
	if (v.type() == typeid(InputCoinbase)) {
		auto &in     = boost::get<InputCoinbase>(v);
		uint8_t type = InputCoinbase::type_tag;
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = InputCoinbase::str_type_tag();
			ser(str_type_tag, s);
		} else
			s.binary(&type, 1);
		ser_members(in, s);
	} else if (v.type() == typeid(InputKey)) {
		auto &in     = boost::get<InputKey>(v);
		uint8_t type = InputKey::type_tag;
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = InputKey::str_type_tag();
			ser(str_type_tag, s);
		} else
			s.binary(&type, 1);
		ser_members(in, s);
	}
}
void ser_members(TransactionOutput &v, ISeria &s, bool is_tx_amethyst) {
	if (s.is_input()) {
		Amount amount = 0;
		if (!is_tx_amethyst)
			seria_kv("amount", amount, s);
		uint8_t type = 0;
		s.object_key("type");
		if (dynamic_cast<seria::JsonInputStream *>(&s)) {
			std::string str_type_tag;
			ser(str_type_tag, s);
			if (str_type_tag == OutputKey::str_type_tag())
				type = OutputKey::type_tag;
			else if (str_type_tag == OutputKey::str_type_tag_auditable())
				type = OutputKey::type_tag_auditable;
			else
				throw std::runtime_error("Deserialization error - unknown output type " + str_type_tag);
		} else
			s.binary(&type, 1);
		switch (type) {
		case OutputKey::type_tag:
		case OutputKey::type_tag_auditable: {
			OutputKey in{};
			in.amount       = amount;
			in.is_auditable = (type == OutputKey::type_tag_auditable);
			ser_members(in, s, is_tx_amethyst);
			v = in;
			break;
		}
		default:
			throw std::runtime_error("Deserialization error - unknown output type " + common::to_string(type));
		}
		return;
	}
	if (v.type() == typeid(OutputKey)) {
		auto &in = boost::get<OutputKey>(v);
		if (!is_tx_amethyst)
			seria_kv("amount", in.amount, s);
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag =
			    in.is_auditable ? OutputKey::str_type_tag_auditable() : OutputKey::str_type_tag();
			ser(str_type_tag, s);
		} else {
			uint8_t type = in.is_auditable ? OutputKey::type_tag_auditable : OutputKey::type_tag;
			s.binary(&type, 1);
		}
		ser_members(in, s, is_tx_amethyst);
	}
}
void ser_members(InputCoinbase &v, ISeria &s) { seria_kv("height", v.height, s); }
void ser_members(InputKey &v, ISeria &s) {
	seria_kv("amount", v.amount, s);
	seria_kv("output_indexes", v.output_indexes, s);
	seria_kv("key_image", v.key_image, s);
}
void ser_members(cn::RingSignatures &v, ISeria &s) { seria_kv("signatures", v.signatures, s); }
void ser_members(cn::RingSignature3 &v, ISeria &s) {
	seria_kv("c0", v.c0, s);
	seria_kv("r", v.r, s);
}

enum { ring_signature_blank_tag = 0xff, ring_signature_normal_tag = 1, ring_signature_halfsize_tag = 2 };
const char ring_signature_blank_tag_str[]    = "blank";
const char ring_signature_normal_tag_str[]   = "normal";
const char ring_signature_halfsize_tag_str[] = "halfsize";

// Usually signatures are parsed in the context of transaction where type is known
// but sometimes we need to send just signatures, so we need to seria them independently

void ser_members(cn::TransactionSignatures &v, ISeria &s) {
	if (s.is_input()) {
		uint8_t type = 0;
		s.object_key("type");
		if (dynamic_cast<seria::JsonInputStream *>(&s)) {
			std::string str_type_tag;
			ser(str_type_tag, s);
			if (str_type_tag == ring_signature_blank_tag_str)
				type = ring_signature_blank_tag;
			else if (str_type_tag == ring_signature_normal_tag_str)
				type = ring_signature_normal_tag;
			else if (str_type_tag == ring_signature_halfsize_tag_str)
				type = ring_signature_halfsize_tag;
			else
				throw std::runtime_error("Deserialization error - unknown ring signature type " + str_type_tag);
		} else
			s.binary(&type, 1);
		switch (type) {
		case ring_signature_blank_tag: {
			v = boost::blank{};
			break;
		}
		case ring_signature_normal_tag: {
			RingSignatures ring_sig;
			ser_members(ring_sig, s);
			v = ring_sig;
			break;
		}
		case ring_signature_halfsize_tag: {
			RingSignature3 ring_sig;
			ser_members(ring_sig, s);
			v = ring_sig;
			break;
		}
		default:
			throw std::runtime_error("Deserialization error - unknown ring signature type " + common::to_string(type));
		}
		return;
	}
	if (v.type() == typeid(boost::blank)) {
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = ring_signature_blank_tag_str;
			ser(str_type_tag, s);
		} else {
			uint8_t type = ring_signature_blank_tag;
			s.binary(&type, 1);
		}
	} else if (v.type() == typeid(RingSignatures)) {
		auto &in = boost::get<RingSignatures>(v);
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = ring_signature_normal_tag_str;
			ser(str_type_tag, s);
		} else {
			uint8_t type = ring_signature_normal_tag;
			s.binary(&type, 1);
		}
		ser_members(in, s);
	} else if (v.type() == typeid(RingSignature3)) {
		auto &in = boost::get<RingSignature3>(v);
		s.object_key("type");
		if (dynamic_cast<seria::JsonOutputStream *>(&s)) {
			std::string str_type_tag = ring_signature_halfsize_tag_str;
			ser(str_type_tag, s);
		} else {
			uint8_t type = ring_signature_halfsize_tag;
			s.binary(&type, 1);
		}
		ser_members(in, s);
	} else
		throw std::runtime_error("Serialization error - unknown ring signature type");
}

// Serializing in the context of transaction - sizes and types are known from transaction prefix
void ser_members(cn::TransactionSignatures &v, ISeria &s, const TransactionPrefix &prefix) {
	const bool is_base = (prefix.inputs.size() == 1) && (prefix.inputs[0].type() == typeid(InputCoinbase));
	if (is_base)
		return;  // No signatures in base transaction
	size_t sig_size           = prefix.inputs.size();
	const bool is_tx_amethyst = (prefix.version >= parameters::TRANSACTION_VERSION_AMETHYST);

	s.object_key("signatures");
	if (is_tx_amethyst) {
		s.begin_object();
		if (s.is_input())
			v = RingSignature3{};
		auto &sigs = boost::get<RingSignature3>(v);
		if (s.is_input())
			sigs.r.resize(sig_size);
		s.object_key("c0");
		ser(sigs.c0, s);
		s.object_key("r");
		s.begin_array(sig_size, true);
		for (size_t i = 0; i < sig_size; ++i) {
			invariant(prefix.inputs[i].type() == typeid(InputKey),
			    "Serialization error: input type wrong for transaction version");
			size_t signature_size = boost::get<InputKey>(prefix.inputs[i]).output_indexes.size();
			if (s.is_input()) {
				sigs.r[i].resize(signature_size);
				s.begin_array(signature_size, true);
				for (crypto::EllipticCurveScalar &sig : sigs.r[i]) {
					ser(sig, s);
				}
				s.end_array();
			} else {
				invariant(signature_size == sigs.r[i].size(), "Serialization error: unexpected signatures size");
				s.begin_array(signature_size, true);
				for (crypto::EllipticCurveScalar &sig : sigs.r[i]) {
					ser(sig, s);
				}
				s.end_array();
			}
		}
		s.end_array();
		s.end_object();
	} else {
		s.begin_array(sig_size, true);
		if (s.is_input())
			v = RingSignatures{};
		auto &sigs = boost::get<RingSignatures>(v);
		if (s.is_input())
			sigs.signatures.resize(sig_size);
		for (size_t i = 0; i < sig_size; ++i) {
			invariant(prefix.inputs[i].type() == typeid(InputKey),
			    "Serialization error: input type wrong for transaction version");
			size_t signature_size = boost::get<InputKey>(prefix.inputs[i]).output_indexes.size();
			if (s.is_input()) {
				sigs.signatures[i].resize(signature_size);
				s.begin_array(signature_size, true);
				for (crypto::Signature &sig : sigs.signatures[i]) {
					ser(sig, s);
				}
				s.end_array();
			} else {
				invariant(
				    signature_size == sigs.signatures[i].size(), "Serialization error: unexpected signatures size");
				s.begin_array(signature_size, true);
				for (crypto::Signature &sig : sigs.signatures[i]) {
					ser(sig, s);
				}
				s.end_array();
			}
		}
		s.end_array();
	}
}

void ser_members(OutputKey &v, ISeria &s, bool is_tx_amethyst) {
	if (is_tx_amethyst)  // We moved amount inside variant part in amethyst
		seria_kv("amount", v.amount, s);
	seria_kv("public_key", v.public_key, s);
	if (is_tx_amethyst)
		seria_kv("encrypted_secret", v.encrypted_secret, s);
}

void ser_members(TransactionPrefix &v, ISeria &s) {
	seria_kv("version", v.version, s);
	const bool is_tx_amethyst = (v.version >= parameters::TRANSACTION_VERSION_AMETHYST);
	seria_kv("unlock_block_or_timestamp", v.unlock_block_or_timestamp, s);
	seria_kv("inputs", v.inputs, s);
	seria_kv("outputs", v.outputs, s, is_tx_amethyst);
	seria_kv("extra", v.extra, s);
}
void ser_members(BaseTransaction &v, ISeria &s) {
	ser_members(static_cast<TransactionPrefix &>(v), s);
	if (v.version >= 2) {
		size_t ignored = 0;
		seria_kv("ignored", ignored, s);
	}
}

// static size_t get_signatures_count(const TransactionInput &input) {
//	struct txin_signature_size_visitor : public boost::static_visitor<size_t> {
//		size_t operator()(const InputCoinbase &) const { return 0; }
//		size_t operator()(const InputKey &txin) const { return txin.output_indexes.size(); }
//	};
//	return boost::apply_visitor(txin_signature_size_visitor(), input);
//}

void ser_members(Transaction &v, ISeria &s) {
	ser_members(static_cast<TransactionPrefix &>(v), s);
	ser_members(v.signatures, s, static_cast<TransactionPrefix &>(v));
}

void ser_members(RootBlock &v, ISeria &s, BlockSeriaType seria_type) {
	seria_kv("major_version", v.major_version, s);

	seria_kv("minor_version", v.minor_version, s);
	seria_kv("timestamp", v.timestamp, s);
	seria_kv("previous_block_hash", v.previous_block_hash, s);
	s.object_key("nonce");
	s.binary(v.nonce, 4);

	if (seria_type == BlockSeriaType::BLOCKHASH || seria_type == BlockSeriaType::LONG_BLOCKHASH) {
		Hash miner_tx_hash = get_root_block_base_transaction_hash(v.base_transaction);
		Hash merkle_root   = crypto::tree_hash_from_branch(
            v.base_transaction_branch.data(), v.base_transaction_branch.size(), miner_tx_hash, nullptr);

		seria_kv("merkle_root", merkle_root, s);
	}
	seria_kv("transaction_count", v.transaction_count, s);
	if (v.transaction_count < 1)
		throw std::runtime_error("Wrong transactions number");

	if (seria_type == BlockSeriaType::LONG_BLOCKHASH)
		return;

	size_t branch_size = crypto_coinbase_tree_depth(v.transaction_count);
	if (!s.is_input()) {
		if (v.base_transaction_branch.size() != branch_size)
			throw std::runtime_error("Wrong miner transaction branch size");
	} else {
		v.base_transaction_branch.resize(branch_size);
	}

	s.object_key("coinbase_transaction_branch");
	size_t btb_size = v.base_transaction_branch.size();
	s.begin_array(btb_size, true);
	for (Hash &hash : v.base_transaction_branch) {
		ser(hash, s);
	}
	s.end_array();

	seria_kv("coinbase_transaction", v.base_transaction, s);

	TransactionExtraMergeMiningTag mm_tag;
	if (!extra_get_merge_mining_tag(v.base_transaction.extra, mm_tag))
		throw std::runtime_error("Can't get extra merge mining tag");
	if (mm_tag.depth > 8 * sizeof(Hash))
		throw std::runtime_error("Wrong merge mining tag depth");

	if (!s.is_input()) {
		if (mm_tag.depth != v.blockchain_branch.size())
			throw std::runtime_error("Blockchain branch size must be equal to merge mining tag depth");
	} else {
		v.blockchain_branch.resize(mm_tag.depth);
	}

	s.object_key("blockchain_branch");
	btb_size = v.blockchain_branch.size();
	s.begin_array(btb_size, true);
	for (Hash &hash : v.blockchain_branch) {
		ser(hash, s);
	}
	s.end_array();
}
/*
    We commented code below (for now) which uses bitmask to save space on enconding
    zero hashes (space saved depends on how many collisions in coins currency ids)

    size_t length = v.cm_merkle_branch.size();
    seria_kv("cm_merkle_branch_length", length, s);
    std::vector<unsigned char> mask((length + 7) / 8);
    if (s.is_input()) {
        v.cm_merkle_branch.resize(length);
        s.object_key("cm_merkle_branch_mask");
        s.binary(mask.data(), mask.size());
        size_t non_zero_count = 0;
        for (size_t i = 0; i != length; ++i)
            if ((mask.at(i / 8) & (1U << (i % 8))) != 0)
                non_zero_count += 1;
        s.object_key("cm_merkle_branch");
        s.begin_array(non_zero_count, true);
        for (size_t i = 0; i != length; ++i) {
            if ((mask.at(i / 8) & (1U << (i % 8))) != 0)
                ser(v.cm_merkle_branch.at(i), s);
            else
                v.cm_merkle_branch.at(i) = Hash{};
        }
        s.end_array();
    } else {
        std::vector<Hash> non_zero_hashes;
        for (size_t i = 0; i != length; ++i)
            if (v.cm_merkle_branch.at(i) != Hash{}) {
                mask.at(i / 8) |= (1U << (i % 8));
                non_zero_hashes.push_back(v.cm_merkle_branch.at(i));
            }
        s.object_key("cm_merkle_branch_mask");
        s.binary(mask.data(), mask.size());
        s.object_key("cm_merkle_branch");
        size_t non_zero_count = non_zero_hashes.size();
        s.begin_array(non_zero_count, true);
        for (Hash &hash : non_zero_hashes) {
            ser(hash, s);
        }
        s.end_array();
    }
*/
void ser_members(crypto::CMBranchElement &v, ISeria &s) {
	s.object_key("depth");
	s.binary(&v.depth, 1);
	seria_kv("hash", v.hash, s);
}

void ser_members(BlockHeader &v,
    ISeria &s,
    BlockSeriaType seria_type,
    BlockBodyProxy body_proxy,
    const crypto::Hash &cm_path) {
	if (seria_type == BlockSeriaType::NORMAL) {
		seria_kv("major_version", v.major_version, s);
		seria_kv("minor_version", v.minor_version, s);
		if (v.major_version == 1) {
			seria_kv("timestamp", v.timestamp, s);
			seria_kv("previous_block_hash", v.previous_block_hash, s);
			v.nonce.resize(4);
			s.object_key("nonce");
			s.binary(v.nonce.data(), 4);
			return;
		}
		if (v.is_merge_mined()) {
			seria_kv("previous_block_hash", v.previous_block_hash, s);
			seria_kv("root_block", v.root_block, s);
			if (s.is_input()) {
				v.nonce.assign(std::begin(v.root_block.nonce), std::end(v.root_block.nonce));
				v.timestamp = v.root_block.timestamp;
			}
			return;
		}
#if bytecoin_ALLOW_CM
		if (v.is_cm_mined()) {
			seria_kv("timestamp", v.timestamp, s);
			seria_kv("previous_block_hash", v.previous_block_hash, s);
			seria_kv("nonce", v.nonce, s);
			seria_kv("cm_merkle_branch", v.cm_merkle_branch, s);
			return;
		}
#endif
		throw std::runtime_error("Unknown block major version " + common::to_string(v.major_version));
	}
	if (v.major_version == 1) {
		seria_kv("major_version", v.major_version, s);
		seria_kv("minor_version", v.minor_version, s);
		seria_kv("timestamp", v.timestamp, s);
		seria_kv("previous_block_hash", v.previous_block_hash, s);
		invariant(v.nonce.size() == 4, "");
		s.object_key("nonce");
		s.binary(v.nonce.data(), 4);
		seria_kv("body_proxy", body_proxy, s);
		return;
	}
	if (v.is_merge_mined()) {
		if (seria_type == BlockSeriaType::LONG_BLOCKHASH) {
			seria_kv("root_block", v.root_block, s, seria_type);
			return;
		}
		seria_kv("major_version", v.major_version, s);
		seria_kv("minor_version", v.minor_version, s);
		seria_kv("previous_block_hash", v.previous_block_hash, s);
		seria_kv("body_proxy", body_proxy, s);
		if (seria_type != BlockSeriaType::PREHASH) {  // BLOCKHASH
			seria_kv("root_block", v.root_block, s, seria_type);
		}
		return;
	}
#if bytecoin_ALLOW_CM
	if (v.is_cm_mined()) {
		if (seria_type == BlockSeriaType::LONG_BLOCKHASH) {
			Hash cm_merkle_root = crypto::tree_hash_from_cm_branch(
			    v.cm_merkle_branch, get_auxiliary_block_header_hash(v, body_proxy), cm_path);
			// We should not allow adding merkle_root_hash twice to the long_hashing_array, so more
			// flexible "pre_nonce" | root | "post_nonce" would be bad (allow mining of sidechains)
			// We select "nonce" to be first, because this would allow compatibility with some weird
			// ASICs which select algorithm based on block major version. Nonce could be made to contain
			// all required bytes and be of appropriate length (39+4+1 bytes for full binary compatibilty)
			s.object_key("nonce");
			s.binary(v.nonce.data(), v.nonce.size());
			seria_kv("cm_merkle_root", cm_merkle_root, s);
			return;
		}
		// Any participating currency must have prehash taken from something with length != 65 bytes
		seria_kv("major_version", v.major_version, s);
		seria_kv("minor_version", v.minor_version, s);
		seria_kv("timestamp", v.timestamp, s);
		seria_kv("previous_block_hash", v.previous_block_hash, s);
		seria_kv("body_proxy", body_proxy, s);
		if (seria_type != BlockSeriaType::PREHASH) {  // BLOCKHASH
			seria_kv("nonce", v.nonce, s);
			seria_kv("cm_merkle_branch", v.cm_merkle_branch, s);
		}
		return;
	}
#endif
	throw std::runtime_error("Unknown block major version " + common::to_string(v.major_version));
}
void ser_members(BlockBodyProxy &v, ISeria &s) {
	seria_kv("transactions_merkle_root", v.transactions_merkle_root, s);
	seria_kv("transaction_count", v.transaction_count, s);
}
void ser_members(BlockTemplate &v, ISeria &s) {
	ser_members(static_cast<BlockHeader &>(v), s);
	seria_kv("coinbase_transaction", v.base_transaction, s);
	seria_kv("transaction_hashes", v.transaction_hashes, s);
}
void ser_members(RawBlock &v, ISeria &s) {
	seria_kv("block", v.block, s);
	seria_kv("txs", v.transactions, s);  // Name important for P2P kv-binary
}
void ser_members(Block &v, ISeria &s) {
	seria_kv("header", v.header, s);
	seria_kv("transactions", v.transactions, s);
}
void ser_members(HardCheckpoint &v, ISeria &s) {
	seria_kv("height", v.height, s);
	seria_kv("hash", v.hash, s);
}
void ser_members(Checkpoint &v, seria::ISeria &s) {
	seria_kv("height", v.height, s);
	seria_kv("hash", v.hash, s);
	seria_kv("key_id", v.key_id, s);
	seria_kv("counter", v.counter, s);
}
void ser_members(SignedCheckpoint &v, seria::ISeria &s) {
	ser_members(static_cast<Checkpoint &>(v), s);
	seria_kv("signature", v.signature, s);
}

}  // namespace seria
