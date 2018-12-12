// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include "BlockChainState.hpp"
#include <condition_variable>
#include <random>
#include <unordered_set>
#include "Config.hpp"
#include "CryptoNoteTools.hpp"
#include "Currency.hpp"
#include "TransactionExtra.hpp"
#include "common/Math.hpp"
#include "common/StringTools.hpp"
#include "common/Varint.hpp"
#include "crypto/crypto.hpp"
#include "platform/Time.hpp"
#include "seria/BinaryInputStream.hpp"
#include "seria/BinaryOutputStream.hpp"

static const std::string KEYIMAGE_PREFIX             = "i";
static const std::string AMOUNT_OUTPUT_PREFIX        = "a";
static const std::string BLOCK_GLOBAL_INDICES_PREFIX = "b";
static const std::string BLOCK_GLOBAL_INDICES_SUFFIX = "g";

static const std::string DIN_PREFIX = "D";

const int chain_reaction = 2;

using namespace cn;
using namespace platform;

typedef std::pair<std::vector<size_t>, std::vector<size_t>> InputDesc;

namespace seria {
void ser_members(IBlockChainState::UnlockTimePublickKeyHeightSpent &v, ISeria &s) {
	seria_kv("unlock_block_or_timestamp", v.unlock_block_or_timestamp, s);
	seria_kv("public_key", v.public_key, s);
	seria_kv("height", v.height, s);
	seria_kv("auditable", v.auditable, s);
	seria_kv("spent", v.spent, s);
	seria_kv("dins", v.dins, s);
}
}  // namespace seria

BlockChainState::PoolTransaction::PoolTransaction(const Transaction &tx, const BinaryArray &binary_tx, Amount fee,
    Timestamp timestamp, const Hash &newest_referenced_block)
    : tx(tx)
    , binary_tx(binary_tx)
    , amount(get_tx_sum_outputs(tx))
    , fee(fee)
    , timestamp(timestamp)
    , newest_referenced_block(newest_referenced_block) {}

void BlockChainState::DeltaState::store_keyimage(const KeyImage &key_image, Height height) {
	invariant(m_keyimages.insert(std::make_pair(key_image, height)).second, common::pod_to_hex(key_image));
}

void BlockChainState::DeltaState::delete_keyimage(const KeyImage &key_image) {
	invariant(m_keyimages.erase(key_image) == 1, common::pod_to_hex(key_image));
}

bool BlockChainState::DeltaState::read_keyimage(const KeyImage &key_image, Height *height) const {
	auto kit = m_keyimages.find(key_image);
	if (kit == m_keyimages.end())
		return m_parent_state->read_keyimage(key_image, height);
	*height = m_block_height;
	return true;
}

size_t BlockChainState::DeltaState::push_amount_output(
    Amount amount, BlockOrTimestamp unlock_time, Height block_height, const PublicKey &pk, bool is_auditable) {
	auto pg  = m_parent_state->next_global_index_for_amount(amount);
	auto &ga = m_global_amounts[amount];
	ga.push_back(std::make_tuple(unlock_time, pk, is_auditable));
	return pg + ga.size() - 1;
}

void BlockChainState::DeltaState::pop_amount_output(
    Amount amount, BlockOrTimestamp unlock_time, const PublicKey &pk, bool is_auditable) {
	std::vector<std::tuple<uint64_t, PublicKey, bool>> &el = m_global_amounts[amount];
	invariant(!el.empty(), "DeltaState::pop_amount_output underflow");
	invariant(
	    std::get<0>(el.back()) == unlock_time && std::get<1>(el.back()) == pk && std::get<2>(el.back()) == is_auditable,
	    "DeltaState::pop_amount_output wrong element");
	el.pop_back();
}

size_t BlockChainState::DeltaState::next_global_index_for_amount(Amount amount) const {
	auto pg  = m_parent_state->next_global_index_for_amount(amount);
	auto git = m_global_amounts.find(amount);
	return (git == m_global_amounts.end()) ? pg : git->second.size() + pg;
}

bool BlockChainState::DeltaState::read_amount_output(
    Amount amount, size_t global_index, UnlockTimePublickKeyHeightSpent *unp) const {
	//	uint32_t pg = m_parent_state->next_global_index_for_amount(amount);
	//	if (global_index < pg)
	return m_parent_state->read_amount_output(amount, global_index, unp);
	//	global_index -= pg;
	//	auto git = m_global_amounts.find(amount);
	//	if (git == m_global_amounts.end() || global_index >= git->second.size())
	//		return false;
	//	unp->unlock_block_or_timestamp = std::get<0>(git->second[global_index]);
	//	unp->public_key                = std::get<1>(git->second[global_index]);
	//	*is_white = std::get<2>(git->second[global_index]);
	//	unp->height                    = m_block_height;
	//	unp->spent = false;  // Spending just created outputs inside mempool or block is prohibited, simplifying logic
	//	return true;
}
// void BlockChainState::DeltaState::spend_output(Amount amount, size_t global_index) {
//	m_spent_outputs.push_back(std::make_pair(amount, global_index));
//}

void BlockChainState::DeltaState::apply(IBlockChainState *parent_state) const {
	for (auto &&ki : m_keyimages)
		parent_state->store_keyimage(ki.first, ki.second);
	for (auto &&amp : m_global_amounts)
		for (auto &&el : amp.second)
			parent_state->push_amount_output(
			    amp.first, std::get<0>(el), m_block_height, std::get<1>(el), std::get<2>(el));
	//	for (auto &&mo : m_spent_outputs)
	//		parent_state->spend_output(mo.first, mo.second);
}

void BlockChainState::DeltaState::clear(Height new_block_height) {
	m_block_height = new_block_height;
	m_keyimages.clear();
	m_global_amounts.clear();
	//	m_spent_outputs.clear();
}

api::BlockHeader BlockChainState::fill_genesis(Hash genesis_bid, const BlockTemplate &g) {
	api::BlockHeader result;
	result.major_version       = g.major_version;
	result.minor_version       = g.minor_version;
	result.previous_block_hash = g.previous_block_hash;
	result.timestamp           = g.timestamp;
	result.binary_nonce        = g.nonce;
	result.hash                = genesis_bid;
	return result;
}

// returns reward for coinbase transaction or fee for non-coinbase one
static Amount validate_semantic(const Currency &currency, uint8_t block_major_version, bool generating,
    const Transaction &tx, bool check_output_key) {
	if (tx.inputs.empty())
		throw ConsensusError("Empty inputs");
	//	TODO - uncomment during next hard fork, finally prohibiting old signatures, outputs without secrets
	//	We cannot do it at once, because mem pool will have v1 transactions during switch

	//	if(block_major_version >= currency.amethyst_block_version && tx.version <
	// currency.amethyst_transaction_version && !generating) // for compatibility, we create v1 coinbase
	// transaction if mining on legacy address
	//		return "WRONG_TRANSACTION_VERSION";
	if (block_major_version < currency.amethyst_block_version && tx.version >= currency.amethyst_transaction_version)
		throw ConsensusError(common::to_string(
		    "Wrong transaction version", int(tx.version), "in block version", int(block_major_version)));
	Amount summary_output_amount = 0;
	for (const auto &output : tx.outputs) {
		Amount amount = 0;
		if (!currency.amount_allowed_in_output(block_major_version, amount))
			throw ConsensusError(common::to_string("Not round amount", amount));
		if (output.type() == typeid(OutputKey)) {
			const auto &key_output = boost::get<OutputKey>(output);
			amount                 = key_output.amount;
			if (check_output_key && !key_isvalid(key_output.public_key))
				throw ConsensusError(common::to_string("Output key not valid elliptic point", key_output.public_key));
			if (tx.version < currency.amethyst_transaction_version && key_output.is_auditable)
				throw ConsensusError(
				    common::to_string("Transaction version", tx.version, "insufficient for output with audit"));
		} else
			throw ConsensusError("Output type unknown");
		if (amount == 0)
			throw ConsensusError("Output amount 0");
		//		if (std::numeric_limits<Amount>::max() - amount < summary_output_amount)
		//			throw ConsensusError("Outputs amounts overflow");
		if (!add_amount(summary_output_amount, amount))
			throw ConsensusError("Outputs amounts overflow");
	}
	Amount summary_input_amount = 0;
	std::unordered_set<KeyImage> ki;
	for (const auto &input : tx.inputs) {
		Amount amount = 0;
		if (input.type() == typeid(InputCoinbase)) {
			if (!generating)
				throw ConsensusError("Coinbase input in non-coinbase transaction");
		} else if (input.type() == typeid(InputKey)) {
			if (generating)
				throw ConsensusError("Key input in coinbase transaction");
			const InputKey &in = boost::get<InputKey>(input);
			amount             = in.amount;
			if (!ki.insert(in.key_image).second)
				throw ConsensusError(common::to_string("Keyimage used twice in same transaction", in.key_image));
			std::vector<size_t> global_indexes;
			if (!relative_output_offsets_to_absolute(&global_indexes, in.output_indexes))
				throw ConsensusError("Output indexes invalid in input");
		} else
			throw ConsensusError("Input type unknown");
		//		if (std::numeric_limits<Amount>::max() - amount < summary_input_amount)
		//			throw ConsensusError("Inputs amounts overflow");
		if (!add_amount(summary_input_amount, amount))
			throw ConsensusError("Outputs amounts overflow");
	}
	if (summary_output_amount > summary_input_amount && !generating)
		throw ConsensusError("Sum of outputs > sum of inputs in non-coinbase transaction");
	//	Types/count of signatures will be checked as a part of signatures check
	//	if (tx.signatures.size() != tx.inputs.size() && !generating)
	//		return "INPUT_UNKNOWN_TYPE";
	//	if (!tx.signatures.empty() && generating)
	//		return "INPUT_UNKNOWN_TYPE";
	if (generating)
		return summary_output_amount;
	return summary_input_amount - summary_output_amount;
}

BlockChainState::BlockChainState(logging::ILogger &log, const Config &config, const Currency &currency, bool read_only)
    : BlockChain(log, config, currency, read_only)
    , m_max_pool_size(config.max_pool_size)
    , m_log_redo_block_timestamp(std::chrono::steady_clock::now()) {
	std::string version;
	m_db.get("$version", version);
	if (version == "B" || version == "1" || version == "2" || version == "3" || version == "4" || version == "5") {
		start_internal_import();
		version = version_current;
		m_db.put("$version", version, false);
		db_commit();
	}
	// Upgrades from 5 should restart internal import if m_internal_import_chain is not empty
	if (version != version_current)
		throw std::runtime_error("Blockchain database format unknown (version=" + version + "), please delete " +
		                         config.get_data_folder() + "/blockchain");
	if (get_tip_height() == (Height)-1) {
		//		Block genesis_block;
		//		genesis_block.header = currency.genesis_block_template;
		RawBlock raw_block;
		raw_block.block = seria::to_binary(currency.genesis_block_template);
		//		invariant(genesis_block.to_raw_block(raw_block), "Genesis block failed to convert into raw block");
		PreparedBlock pb(std::move(raw_block), m_currency, nullptr);
		api::BlockHeader info;
		invariant(add_block(pb, &info, std::string()), "Genesis block failed to add");
	}
	BlockChainState::tip_changed();
	m_log(logging::INFO) << "BlockChainState::BlockChainState height=" << get_tip_height()
	                     << " cumulative_difficulty=" << get_tip_cumulative_difficulty() << " bid=" << get_tip_bid()
	                     << std::endl;
	build_blods();
	DB::Cursor cur2 = m_db.rbegin(DIN_PREFIX);
	m_next_nz_input_index =
	    cur2.end() ? 0 : common::integer_cast<size_t>(common::read_varint_sqlite4(cur2.get_suffix())) + 1;
}

void BlockChainState::check_standalone_consensus(
    const PreparedBlock &pb, api::BlockHeader *info, const api::BlockHeader &prev_info, bool check_pow) const {
	if (pb.error)  // Some semantic checks are in PreparedBlock::prepare
		throw pb.error.get();
	const auto &block = pb.block;
	if (block.transactions.size() != block.header.transaction_hashes.size() ||
	    block.transactions.size() != pb.raw_block.transactions.size())
		throw ConsensusError("Wrong transaction count in block template");
	// Timestamps are within reason
	if (get_tip_bid() == prev_info.hash)  // Optimization for most common case
		info->timestamp_median = m_next_median_timestamp;
	else
		info->timestamp_median = calculate_next_median_timestamp(prev_info);
	auto now = platform::now_unix_timestamp();  // It would be better to pass now through Node
	if (block.header.timestamp > now + m_currency.block_future_time_limit)
		throw ConsensusError("Timestamp too far in future");
	if (block.header.timestamp < info->timestamp_median)
		throw ConsensusError("Timestamp too far in past");
	// Block versions
	const bool is_amethyst = block.header.major_version >= m_currency.amethyst_block_version;
	const auto body_proxy  = get_body_proxy_from_template(block.header);

	uint8_t should_be_major_mm = 0, should_be_major_cm = 0, might_be_minor = 0;
	if (!fill_next_block_versions(prev_info, false, &should_be_major_mm, &should_be_major_cm, &might_be_minor))
		throw ConsensusError("Block does not pass through last hard checkpoint");
	if (block.header.major_version != should_be_major_mm && block.header.major_version != should_be_major_cm)
		throw ConsensusError(common::to_string("Block version wrong", int(block.header.major_version), "instead of",
		    int(should_be_major_mm), "or", int(should_be_major_cm)));

	// Object sizes ok
	size_t cumulative_size = 0;
	for (size_t i = 0; i != pb.raw_block.transactions.size(); ++i)
		cumulative_size += pb.raw_block.transactions.at(i).size();
	if (is_amethyst) {  // We care only about single limit - block size
		if (!extra_get_block_capacity_vote(block.header.base_transaction.extra, &info->block_capacity_vote))
			throw ConsensusError("No block capacity vote");
		if (info->block_capacity_vote < m_currency.block_capacity_vote_min ||
		    info->block_capacity_vote > m_currency.block_capacity_vote_max)
			throw ConsensusError(common::to_string("Invalid block capacity vote", info->block_capacity_vote,
			    "should be >=", m_currency.block_capacity_vote_min, "and <=", m_currency.block_capacity_vote_max));
		if (get_tip_bid() == prev_info.hash)  // Optimization for most common case
			info->block_capacity_vote_median = m_next_median_block_capacity_vote;
		else
			info->block_capacity_vote_median = calculate_next_median_block_capacity_vote(prev_info);
		info->transactions_size = pb.coinbase_tx_size + cumulative_size;
		if (info->transactions_size > info->block_capacity_vote_median)
			throw ConsensusError(common::to_string("Block size too big, transactions size", info->transactions_size,
			    "should be <=", info->block_capacity_vote_median));
		if (pb.block_header_size > m_currency.max_header_size)
			throw ConsensusError(common::to_string(
			    "Header size too big,", pb.block_header_size, "should be <=", m_currency.max_header_size));
		info->block_size = pb.block_header_size + common::get_varint_data(pb.raw_block.transactions.size()).size();
		info->block_size += info->transactions_size;
	} else {
		if (get_tip_bid() == prev_info.hash)  // Optimization for most common case
			info->size_median = m_next_median_size;
		else
			info->size_median = calculate_next_median_size(prev_info);
		auto next_minimum_size_median = m_currency.get_minimum_size_median(block.header.major_version);
		info->effective_size_median   = std::max(info->size_median, next_minimum_size_median);

		info->transactions_size = pb.coinbase_tx_size + cumulative_size;
		info->block_size        = pb.raw_block.block.size() + cumulative_size;
		// block_size not used in consensus calcs, we would change it to match definition in if() above
		// but then some block explorers will not be happy due to change.

		const auto max_transactions_cumulative_size = m_currency.max_block_transactions_cumulative_size(info->height);
		if (info->transactions_size > max_transactions_cumulative_size)
			throw ConsensusError(common::to_string("Cumulative block transactions size too big,",
			    info->transactions_size, "should be <=", max_transactions_cumulative_size));
		if (info->transactions_size > info->effective_size_median * 2)
			throw ConsensusError(common::to_string("Cumulative block transactions size too big,",
			    info->transactions_size, "should be <=", info->effective_size_median * 2));
		if (block.header.is_merge_mined() && pb.parent_block_size > m_currency.max_header_size)
			throw ConsensusError(common::to_string(
			    "Root block size too big,", pb.parent_block_size, "should be <=", m_currency.max_header_size));
	}
	if (block.header.is_merge_mined()) {
		TransactionExtraMergeMiningTag mm_tag;
		if (!extra_get_merge_mining_tag(block.header.root_block.base_transaction.extra, mm_tag))
			throw ConsensusError("No merge mining tag");
		if (mm_tag.depth != block.header.root_block.blockchain_branch.size())
			throw ConsensusError(common::to_string("Wrong merge mining depth,", mm_tag.depth, " should be ",
			    block.header.root_block.blockchain_branch.size()));
		if (block.header.root_block.blockchain_branch.size() > 8 * sizeof(Hash))
			throw ConsensusError(common::to_string("Too big merge mining depth,",
			    block.header.root_block.blockchain_branch.size(), "should be <=", 8 * sizeof(Hash)));
		Hash aux_blocks_merkle_root = crypto::tree_hash_from_branch(block.header.root_block.blockchain_branch.data(),
		    block.header.root_block.blockchain_branch.size(), get_auxiliary_block_header_hash(block.header, body_proxy),
		    &m_currency.genesis_block_hash);
		if (aux_blocks_merkle_root != mm_tag.merkle_root)
			throw ConsensusError(common::to_string(
			    "Wrong merge mining merkle root, tag", mm_tag.merkle_root, "actual", aux_blocks_merkle_root));
	}
	if (block.header.is_cm_mined()) {
		if (!crypto::cm_branch_valid(block.header.cm_merkle_branch))
			throw ConsensusError("CM branch invalid");
	}
	if (block.header.base_transaction.inputs.size() != 1)
		throw ConsensusError(common::to_string(
		    "Coinbase transaction input count wrong,", block.header.base_transaction.inputs.size(), "should be 1"));
	if (block.header.base_transaction.inputs[0].type() != typeid(InputCoinbase))
		throw ConsensusError("Coinbase transaction input type wrong");
	{
		const auto coinbase_input = boost::get<InputCoinbase>(block.header.base_transaction.inputs[0]);
		if (coinbase_input.height != info->height)
			throw ConsensusError(common::to_string(
			    "Coinbase transaction wrong input height,", coinbase_input.height, "should be", info->height));
	}
	if (!is_amethyst && block.header.base_transaction.unlock_block_or_timestamp !=
	                        info->height + m_currency.mined_money_unlock_window) {
		throw ConsensusError(common::to_string("Coinbase transaction wrong unlock time,",
		    block.header.base_transaction.unlock_block_or_timestamp, "should be",
		    info->height + m_currency.mined_money_unlock_window));
	}
	const bool check_keys = m_config.paranoid_checks || !m_currency.is_in_hard_checkpoint_zone(info->height);
	const Amount miner_reward =
	    validate_semantic(m_currency, block.header.major_version, true, block.header.base_transaction, check_keys);
	{
		std::vector<Timestamp> timestamps;
		std::vector<CumulativeDifficulty> difficulties;
		const Height blocks_count = m_currency.difficulty_windows_plus_lag();
		timestamps.reserve(blocks_count);
		difficulties.reserve(blocks_count);
		for_each_reversed_tip_segment(prev_info, blocks_count, false, [&](const api::BlockHeader &header) {
			timestamps.push_back(header.timestamp);
			difficulties.push_back(header.cumulative_difficulty);
		});
		std::reverse(timestamps.begin(), timestamps.end());
		std::reverse(difficulties.begin(), difficulties.end());
		info->difficulty = m_currency.next_effective_difficulty(block.header.major_version, timestamps, difficulties);
		info->cumulative_difficulty = prev_info.cumulative_difficulty + info->difficulty;
	}

	info->transactions_fee = 0;
	for (auto &&tx : pb.block.transactions) {
		const Amount tx_fee = validate_semantic(m_currency, block.header.major_version, false, tx, check_keys);
		info->transactions_fee += tx_fee;
	}

	if (is_amethyst) {
		info->base_reward = m_currency.get_base_block_reward(
		    block.header.major_version, info->height, prev_info.already_generated_coins);
		info->reward                  = info->base_reward + info->transactions_fee;
		info->already_generated_coins = prev_info.already_generated_coins + info->base_reward;
	} else {
		SignedAmount emission_change = 0;
		info->base_reward            = m_currency.get_block_reward(block.header.major_version, info->height,
            info->effective_size_median, 0, prev_info.already_generated_coins, 0, &emission_change);
		info->reward =
		    m_currency.get_block_reward(block.header.major_version, info->height, info->effective_size_median,
		        info->transactions_size, prev_info.already_generated_coins, info->transactions_fee, &emission_change);
		info->already_generated_coins = prev_info.already_generated_coins + emission_change;
	}

	if (miner_reward != info->reward)
		throw ConsensusError(common::to_string("Block reward mismatch,", miner_reward, "should be", info->reward));
	info->already_generated_transactions = prev_info.already_generated_transactions + block.transactions.size() + 1;
	if (m_currency.is_in_hard_checkpoint_zone(info->height)) {
		bool is_checkpoint;
		if (!m_currency.check_hard_checkpoint(info->height, info->hash, is_checkpoint))
			throw ConsensusError(
			    common::to_string("Block does not pass through hard checkpoint at height", info->height));
		return;
	}
	if (!check_pow && !m_config.paranoid_checks)
		return;
	Hash long_hash = pb.long_block_hash;
	if (long_hash == Hash{}) {  // We did not calculate this long hash in parallel
		auto ba   = m_currency.get_block_long_hashing_data(block.header, body_proxy);
		long_hash = m_hash_crypto_context.cn_slow_hash(ba.data(), ba.size());
	}
	if (!check_hash(long_hash, info->difficulty))
		throw ConsensusError("Proof of work too weak");
}
void BlockChainState::fill_statistics(api::cnd::GetStatistics::Response &res) const {
	BlockChain::fill_statistics(res);
	res.transaction_pool_count               = m_memory_state_tx.size();
	res.transaction_pool_size                = m_memory_state_total_size;
	res.transaction_pool_max_size            = m_max_pool_size;
	res.transaction_pool_lowest_fee_per_byte = minimum_pool_fee_per_byte(false);
}

Timestamp BlockChainState::calculate_next_median_timestamp(const api::BlockHeader &prev_info) const {
	std::vector<Timestamp> timestamps;
	auto timestamp_check_window = m_currency.timestamp_check_window(prev_info.major_version);
	timestamps.reserve(timestamp_check_window);
	for_each_reversed_tip_segment(prev_info, timestamp_check_window, false,
	    [&](const api::BlockHeader &header) { timestamps.push_back(header.timestamp); });
	if (timestamps.size() >= timestamp_check_window)
		return common::median_value(&timestamps);  // sorts timestamps
	return 0;
}

size_t BlockChainState::calculate_next_median_size(const api::BlockHeader &prev_info) const {
	std::vector<size_t> last_transactions_sizes;
	last_transactions_sizes.reserve(m_currency.median_block_size_window);
	for_each_reversed_tip_segment(prev_info, m_currency.median_block_size_window, true,
	    [&](const api::BlockHeader &header) { last_transactions_sizes.push_back(header.transactions_size); });
	return common::median_value(&last_transactions_sizes);
}

size_t BlockChainState::calculate_next_median_block_capacity_vote(const api::BlockHeader &prev_info) const {
	std::vector<size_t> last_blocks_sizes;
	last_blocks_sizes.reserve(m_currency.block_capacity_vote_window);
	for_each_reversed_tip_segment(
	    prev_info, m_currency.block_capacity_vote_window, true, [&](const api::BlockHeader &header) {
		    if (header.major_version >= m_currency.amethyst_block_version)
			    last_blocks_sizes.push_back(header.block_capacity_vote);
	    });
	if (last_blocks_sizes.empty())
		return m_currency.block_capacity_vote_min;
	return common::median_value(&last_blocks_sizes);
}

void BlockChainState::tip_changed() {
	m_next_median_timestamp           = calculate_next_median_timestamp(get_tip());
	m_next_median_size                = calculate_next_median_size(get_tip());
	m_next_median_block_capacity_vote = calculate_next_median_block_capacity_vote(get_tip());
}

void BlockChainState::create_mining_block_template(const Hash &parent_bid, const AccountAddress &adr,
    const BinaryArray &extra_nonce, BlockTemplate *b, Difficulty *difficulty, Height *height,
    size_t *reserved_back_offset) const {
	api::BlockHeader parent_info;
	if (!get_header(parent_bid, &parent_info))
		throw std::runtime_error("Attempt to mine from block we do not have");
	*height                  = parent_info.height + 1;
	*b                       = BlockTemplate{};
	uint8_t major_version_cm = 0;
	if (!fill_next_block_versions(parent_info, false, &b->major_version, &major_version_cm, &b->minor_version))
		throw std::runtime_error(
		    "Mining of block in chain not passing through last hard checkpoint is not possible (will not be accepted by network anyway)");
	const bool is_amethyst = b->major_version >= m_currency.amethyst_block_version;

	clear_mining_transactions();  // We periodically forget transactions for old blocks we gave as templates
	{
		std::vector<Timestamp> timestamps;
		std::vector<CumulativeDifficulty> difficulties;
		const Height blocks_count = m_currency.difficulty_windows_plus_lag();
		timestamps.reserve(blocks_count);
		difficulties.reserve(blocks_count);
		for_each_reversed_tip_segment(parent_info, blocks_count, false, [&](const api::BlockHeader &header) {
			timestamps.push_back(header.timestamp);
			difficulties.push_back(header.cumulative_difficulty);
		});
		std::reverse(timestamps.begin(), timestamps.end());
		std::reverse(difficulties.begin(), difficulties.end());
		//		timestamps.reserve(blocks_count);
		//		difficulties.reserve(blocks_count);
		//		auto timestamps_window = get_tip_segment(parent_info, blocks_count, false);
		//		for (auto it = timestamps_window.begin(); it != timestamps_window.end(); ++it) {
		//			timestamps.push_back(it->timestamp);
		//			difficulties.push_back(it->cumulative_difficulty);
		//		}
		*difficulty = m_currency.next_effective_difficulty(b->major_version, timestamps, difficulties);
	}
	b->nonce.resize(4);
	if (b->is_merge_mined()) {
		b->root_block.major_version     = 1;
		b->root_block.minor_version     = 0;
		b->root_block.transaction_count = 1;

		extra_add_merge_mining_tag(b->root_block.base_transaction.extra, TransactionExtraMergeMiningTag{});
	}

	b->previous_block_hash                = parent_bid;
	const Timestamp next_median_timestamp = calculate_next_median_timestamp(parent_info);
	b->root_block.timestamp               = std::max(platform::now_unix_timestamp(), next_median_timestamp);
	b->timestamp                          = b->root_block.timestamp;

	size_t max_txs_size          = 0;
	size_t effective_size_median = 0;

	if (is_amethyst) {
		const size_t next_median_block_capacity_vote = calculate_next_median_block_capacity_vote(parent_info);
		max_txs_size = next_median_block_capacity_vote - m_currency.miner_tx_blob_reserved_size - extra_nonce.size();
	} else {
		const size_t next_median_size         = calculate_next_median_size(parent_info);
		const size_t next_minimum_size_median = m_currency.get_minimum_size_median(b->major_version);
		effective_size_median                 = std::max(next_median_size, next_minimum_size_median);
		const auto max_consensus_transactions_size =
		    std::min(m_currency.max_block_transactions_cumulative_size(*height), 2 * effective_size_median);
		max_txs_size = max_consensus_transactions_size - m_currency.miner_tx_blob_reserved_size - extra_nonce.size();
	}

	std::vector<Hash> pool_hashes;
	for (auto &&msf : m_memory_state_fee_tx)
		pool_hashes.push_back(msf.second);
	size_t txs_size = 0;
	Amount txs_fee  = 0;
	DeltaState memory_state(*height, b->timestamp, next_median_timestamp, this);
	//	Amount base_reward = m_currency.get_block_reward(
	//	    b->major_version, *height, effective_size_median, 0, parent_info.already_generated_coins, 0);

	for (; !pool_hashes.empty(); pool_hashes.pop_back()) {
		auto tit = m_memory_state_tx.find(pool_hashes.back());
		if (tit == m_memory_state_tx.end()) {
			m_log(logging::ERROR) << "Transaction " << pool_hashes.back() << " is in pool index, but not in pool";
			//			assert(false);
			continue;
		}
		const size_t tx_size = tit->second.binary_tx.size();
		const Amount tx_fee  = tit->second.fee;
		if (txs_size + tx_size > max_txs_size)
			continue;
		BlockGlobalIndices global_indices;
		Height conflict_height = 0;
		try {  // double-check that transcations can be added to block
			redo_transaction(b->major_version, false, tit->second.tx, &memory_state, &global_indices, nullptr, true);
		} catch (const ConsensusError &ex) {
			m_log(logging::ERROR) << "Transaction " << tit->first
			                      << " is in pool, but could not be redone what=" << common::what(ex)
			                      << " Conflict height=" << conflict_height << std::endl;
			continue;
		}
		if (!is_amethyst && txs_size + tx_size > effective_size_median)
			continue;  // Effective median size will not grow anyway
		txs_size += tx_size;
		txs_fee += tx_fee;
		b->transaction_hashes.emplace_back(tit->first);
		m_mining_transactions.erase(tit->first);  // We want ot update height to most recent
		m_mining_transactions.insert(std::make_pair(tit->first, std::make_pair(tit->second.binary_tx, *height)));
		m_log(logging::TRACE) << "Transaction " << tit->first << " included to block template";
	}

	if (is_amethyst) {
		// Vote for larger blocks if pool is full of expensive transactions
		Amount desired_fee_per_byte = 100;
		size_t block_capacity_vote  = 0;
		for (auto fit = m_memory_state_fee_tx.rbegin(); fit != m_memory_state_fee_tx.rend(); ++fit) {
			if (fit->first < desired_fee_per_byte)
				break;
			auto tit = m_memory_state_tx.find(fit->second);
			invariant(tit != m_memory_state_tx.end(), "Memory pool corrupted");
			block_capacity_vote += tit->second.binary_tx.size();
		}
		block_capacity_vote += m_currency.block_capacity_vote_min / 2;  // A bit of space for cheaper transactions
		block_capacity_vote = std::max(block_capacity_vote, m_currency.block_capacity_vote_min);
		block_capacity_vote = std::min(block_capacity_vote, m_currency.block_capacity_vote_max);
		Amount block_reward =
		    txs_fee + m_currency.get_base_block_reward(b->major_version, *height, parent_info.already_generated_coins);
		b->base_transaction = m_currency.construct_miner_tx(b->major_version, *height, block_reward, adr);
		extra_add_block_capacity_vote(b->base_transaction.extra, block_capacity_vote);
		if (!extra_nonce.empty())
			extra_add_nonce(b->base_transaction.extra, extra_nonce);
		*reserved_back_offset =
		    common::get_varint_data(b->transaction_hashes.size()).size() + sizeof(Hash) * b->transaction_hashes.size();
		return;
	}
	// two-phase miner transaction generation: we don't know exact block size
	// until we prepare block, but we don't know reward until we know
	// block size, so first miner transaction generated with fake amount of money,
	// and with phase we know think we know expected block size
	// make blocks coin-base tx looks close to real coinbase tx to get truthful blob size
	size_t cumulative_size   = txs_size;
	const size_t TRIES_COUNT = 11;
	for (size_t try_count = 0; try_count < TRIES_COUNT; ++try_count) {
		Amount block_reward = m_currency.get_block_reward(b->major_version, *height, effective_size_median,
		    cumulative_size, parent_info.already_generated_coins, txs_fee);
		b->base_transaction = m_currency.construct_miner_tx(b->major_version, *height, block_reward, adr);
		if (!extra_nonce.empty())
			extra_add_nonce(b->base_transaction.extra, extra_nonce);
		size_t extra_size_without_delta = b->base_transaction.extra.size();
		size_t coinbase_blob_size       = seria::binary_size(b->base_transaction);
		if (coinbase_blob_size + txs_size > cumulative_size) {
			cumulative_size = txs_size + coinbase_blob_size;
			continue;
		}
		if (coinbase_blob_size + txs_size < cumulative_size) {
			size_t delta = cumulative_size - (coinbase_blob_size + txs_size);
			common::append(b->base_transaction.extra, delta, 0);
			// here could be 1 byte difference, because of extra field counter is
			// varint, and it can become from
			// 1-byte len to 2-bytes len.
			if (cumulative_size != txs_size + seria::binary_size(b->base_transaction)) {
				invariant(cumulative_size + 1 == txs_size + seria::binary_size(b->base_transaction), "miner_tx case 1");
				b->base_transaction.extra.resize(b->base_transaction.extra.size() - 1);
				if (cumulative_size != txs_size + seria::binary_size(b->base_transaction)) {
					// ooh, not lucky, -1 makes varint-counter size smaller, in that case
					// we continue to grow with cumulative_size
					m_log(logging::TRACE)
					    << logging::BrightRed << "Miner tx creation have no luck with delta_extra size = " << delta
					    << " and " << delta - 1;
					cumulative_size += delta - 1;
					continue;
				}
				m_log(logging::TRACE) << logging::BrightGreen
				                      << "Setting extra for block: " << b->base_transaction.extra.size()
				                      << ", try_count=" << try_count;
			}
		}
		*reserved_back_offset =
		    common::get_varint_data(b->transaction_hashes.size()).size() + sizeof(Hash) * b->transaction_hashes.size();
		*reserved_back_offset += b->base_transaction.extra.size() - extra_size_without_delta;
		invariant(cumulative_size == txs_size + seria::binary_size(b->base_transaction), "miner_tx case 2");
		return;
	}
	throw std::runtime_error("Failed to create_block_template with " + common::to_string(TRIES_COUNT) + " attempts");
}

bool BlockChainState::add_mined_block(
    const BinaryArray &raw_block_template, RawBlock *raw_block, api::BlockHeader *info) {
	BlockTemplate block_template;
	seria::from_binary(block_template, raw_block_template);
	raw_block->block = std::move(raw_block_template);

	raw_block->transactions.reserve(block_template.transaction_hashes.size());
	raw_block->transactions.clear();
	for (const auto &tx_hash : block_template.transaction_hashes) {
		auto tit                     = m_memory_state_tx.find(tx_hash);
		const BinaryArray *binary_tx = nullptr;
		if (tit != m_memory_state_tx.end())
			binary_tx = &(tit->second.binary_tx);
		else {
			auto tit2 = m_mining_transactions.find(tx_hash);
			if (tit2 == m_mining_transactions.end()) {
				m_log(logging::WARNING) << "The transaction " << tx_hash
				                        << " is absent in transaction pool on submit mined block";
				return false;
			}
			binary_tx = &(tit2->second.first);
		}
		raw_block->transactions.emplace_back(*binary_tx);
	}
	PreparedBlock pb(std::move(*raw_block), m_currency, nullptr);
	*raw_block = pb.raw_block;
	return add_block(pb, info, "json_rpc");
}

void BlockChainState::clear_mining_transactions() const {
	for (auto tit = m_mining_transactions.begin(); tit != m_mining_transactions.end();)
		if (get_tip_height() > tit->second.second + 10)  // Remember used txs for some number of blocks
			tit = m_mining_transactions.erase(tit);
		else
			++tit;
}

Amount BlockChainState::minimum_pool_fee_per_byte(bool zero_if_not_full, Hash *minimal_tid) const {
	if (m_memory_state_fee_tx.empty()) {
		if (minimal_tid)
			*minimal_tid = Hash{};
		return 0;
	}
	if (zero_if_not_full && m_memory_state_total_size < m_max_pool_size) {
		if (minimal_tid)
			*minimal_tid = Hash{};
		return 0;
	}
	auto be = m_memory_state_fee_tx.begin();
	if (minimal_tid)
		*minimal_tid = be->second;
	return be->first;
}

void BlockChainState::on_reorganization(
    const std::map<Hash, std::pair<Transaction, BinaryArray>> &undone_transactions, bool undone_blocks) {
	// TODO - remove/add only those transactions that could have their referenced output keys changed
	if (undone_blocks) {
		PoolTransMap old_memory_state_tx;
		std::swap(old_memory_state_tx, m_memory_state_tx);
		m_memory_state_ki_tx.clear();
		m_memory_state_fee_tx.clear();
		m_memory_state_total_size = 0;
		for (auto &&msf : old_memory_state_tx) {
			try {
				add_transaction(msf.first, msf.second.tx, msf.second.binary_tx, true, std::string());
			} catch (const std::exception &) {  // Just skip now invalid transactions
			}
		}
	}
	for (auto ud : undone_transactions) {
		try {
			add_transaction(ud.first, ud.second.first, ud.second.second, true, std::string());
		} catch (const std::exception &) {  // Just skip now invalid transactions
		}
	}
	m_tx_pool_version = 2;  // add_transaction will erroneously increase
}

std::vector<TransactionDesc> BlockChainState::sync_pool(
    const std::pair<Amount, Hash> &from, const std::pair<Amount, Hash> &to, size_t max_count) const {
	std::vector<TransactionDesc> result;
	auto sit = m_memory_state_fee_tx.lower_bound(from);
	if (sit != m_memory_state_fee_tx.end()) {
		if (*sit != from)
			++sit;
	}
	while (sit != m_memory_state_fee_tx.begin()) {
		--sit;
		if (result.size() > max_count || *sit <= to)
			break;
		TransactionDesc desc;
		desc.hash = sit->second;
		auto tit  = m_memory_state_tx.find(desc.hash);
		invariant(tit != m_memory_state_tx.end(), "");
		desc.fee                     = tit->second.fee;
		desc.size                    = tit->second.binary_tx.size();
		desc.newest_referenced_block = tit->second.newest_referenced_block;
		result.push_back(desc);
	}
	return result;
}

bool BlockChainState::add_transaction(const Hash &tid, const Transaction &tx, const BinaryArray &binary_tx,
    bool check_sigs, const std::string &source_address) {
	if (m_memory_state_tx.count(tid) != 0) {
		m_archive.add(Archive::TRANSACTION, binary_tx, tid, source_address);
		return false;  // AddTransactionResult::ALREADY_IN_POOL;
	}
	//	std::cout << "add_transaction " << tid << std::endl;
	const size_t my_size         = binary_tx.size();
	const Amount my_fee          = cn::get_tx_fee(tx);
	const Amount my_fee_per_byte = my_fee / my_size;
	Hash minimal_tid;
	Amount minimal_fee = minimum_pool_fee_per_byte(false, &minimal_tid);
	// Invariant is if 1 byte of cheapest transaction fits, then all transaction fits
	if (m_memory_state_total_size >= m_max_pool_size && my_fee_per_byte < minimal_fee)
		return false;  // AddTransactionResult::INCREASE_FEE;
	// Deterministic behaviour here and below so tx pools have tendency to stay the same
	if (m_memory_state_total_size >= m_max_pool_size && my_fee_per_byte == minimal_fee && tid < minimal_tid)
		return false;  // AddTransactionResult::INCREASE_FEE;
	for (const auto &input : tx.inputs) {
		if (input.type() == typeid(InputKey)) {
			const InputKey &in = boost::get<InputKey>(input);
			auto tit           = m_memory_state_ki_tx.find(in.key_image);
			if (tit == m_memory_state_ki_tx.end())
				continue;
			const PoolTransaction &other_tx = m_memory_state_tx.at(tit->second);
			const Amount other_fee_per_byte = other_tx.fee_per_byte();
			if (my_fee_per_byte < other_fee_per_byte)
				return false;  // AddTransactionResult::INCREASE_FEE;
			if (my_fee_per_byte == other_fee_per_byte && tid < tit->second)
				return false;  // AddTransactionResult::INCREASE_FEE;
			break;  // Can displace another transaction from the pool, Will have to make heavy-lifting for this tx
		}
	}
	for (const auto &input : tx.inputs) {
		if (input.type() == typeid(InputKey)) {
			const InputKey &in     = boost::get<InputKey>(input);
			Height conflict_height = 0;
			if (read_keyimage(in.key_image, &conflict_height)) {
				throw ConsensusErrorOutputSpent("Output already spent", in.key_image, conflict_height);
			}
		}
	}
	const Amount my_fee3 =
	    validate_semantic(m_currency, get_tip().major_version, false, tx, m_config.paranoid_checks || check_sigs);
	//	if (!validate_result.empty()) {
	//		m_log(logging::WARNING) << "add_transaction validation failed " << validate_result << " in transaction " <<
	// tid << std::endl;
	//		return AddTransactionResult::BAN;
	//	}
	DeltaState memory_state(get_tip_height() + 1, get_tip().timestamp, get_tip().timestamp_median, this);
	BlockGlobalIndices global_indices;
	Hash newest_referenced_bid;
	redo_transaction(
	    get_tip().major_version, false, tx, &memory_state, &global_indices, &newest_referenced_bid, check_sigs);
	//	if (!redo_result.empty()) {
	//		m_log(logging::TRACE) << "add_transaction redo failed " << redo_result << " in transaction " << tid
	//		                      << std::endl;
	//		return AddTransactionResult::FAILED_TO_REDO;  // Not a ban because reorg can change indices
	//	}
	if (my_fee != my_fee3)
		m_log(logging::ERROR) << "Inconsistent fees " << my_fee << ", " << my_fee3 << " in transaction " << tid
		                      << std::endl;
	// Only good transactions are recorded in tx_first_seen, because they require
	// space there
	//	update_first_seen_timestamp(tid, unlock_timestamp);
	for (auto &&ki : memory_state.get_keyimages()) {
		auto tit = m_memory_state_ki_tx.find(ki.first);
		if (tit == m_memory_state_ki_tx.end())
			continue;
		const PoolTransaction &other_tx = m_memory_state_tx.at(tit->second);
		const Amount other_fee_per_byte = other_tx.fee_per_byte();
		if (my_fee_per_byte < other_fee_per_byte)
			return false;  // AddTransactionResult::INCREASE_FEE;  // Never because checked above
		if (my_fee_per_byte == other_fee_per_byte && tid < tit->second)
			return false;  // AddTransactionResult::INCREASE_FEE;  // Never because checked above
		remove_from_pool(tit->second);
	}
	bool all_inserted = true;
	for (auto &&ki : memory_state.get_keyimages()) {
		if (!m_memory_state_ki_tx.insert(std::make_pair(ki.first, tid)).second)
			all_inserted = false;
	}
	const auto now = platform::now_unix_timestamp();
	if (!m_memory_state_tx
	         .insert(std::make_pair(tid, PoolTransaction(tx, binary_tx, my_fee, now, newest_referenced_bid)))
	         .second)
		all_inserted = false;
	if (!m_memory_state_fee_tx.insert(std::make_pair(my_fee_per_byte, tid)).second)
		all_inserted = false;
	// insert all before throw
	invariant(all_inserted, "memory_state_fee_tx empty");
	m_memory_state_total_size += my_size;
	while (m_memory_state_total_size > m_max_pool_size) {
		invariant(!m_memory_state_fee_tx.empty(), "memory_state_fee_tx empty");
		//		auto &be = m_memory_state_fee_tx.begin()->second;
		//		invariant(!be.empty(), "memory_state_fee_tx empty set");
		Hash rhash                        = m_memory_state_fee_tx.begin()->second;
		const PoolTransaction &minimal_tx = m_memory_state_tx.at(rhash);
		if (m_memory_state_total_size < m_max_pool_size + minimal_tx.binary_tx.size())
			break;  // Removing would diminish pool below max size
		remove_from_pool(rhash);
	}
	auto min_size = m_memory_state_fee_tx.empty()
	                    ? 0
	                    : m_memory_state_tx.at(m_memory_state_fee_tx.begin()->second).binary_tx.size();
	auto min_fee_per_byte = m_memory_state_fee_tx.empty() ? 0 : m_memory_state_fee_tx.begin()->first;
	//	if( m_memory_state_total_size-min_size >= m_max_pool_size)
	//		std::cout << "Aha" << std::endl;
	m_log(logging::INFO) << "Added transaction with hash=" << tid << " size=" << my_size << " fee=" << my_fee
	                     << " fee/byte=" << my_fee_per_byte << " current_pool_size=("
	                     << m_memory_state_total_size - min_size << "+" << min_size << ")=" << m_memory_state_total_size
	                     << " count=" << m_memory_state_tx.size() << " min fee/byte=" << min_fee_per_byte << std::endl;
	m_archive.add(Archive::TRANSACTION, binary_tx, tid, source_address);
	//	for(auto && bb : m_memory_state_fee_tx)
	//		for(auto ff : bb.second){
	//			const PoolTransaction &other_tx = m_memory_state_tx.at(ff);
	//			std::cout << "\t" << other_tx.fee_per_byte() << "\t" << other_tx.binary_tx.size() << "\t" <<
	// common::pod_to_hex(ff) << std::endl;
	//		}
	m_tx_pool_version += 1;
	return true;
}

bool BlockChainState::get_largest_referenced_height(const TransactionPrefix &transaction, Height *block_height) const {
	std::map<Amount, size_t> largest_indices;  // Do not check same used amount twice
	size_t input_index = 0;
	for (const auto &input : transaction.inputs) {
		if (input.type() == typeid(InputKey)) {
			const InputKey &in = boost::get<InputKey>(input);
			if (in.output_indexes.empty())
				return false;  // semantics invalid
			size_t largest_index = in.output_indexes[0];
			for (size_t i = 1; i < in.output_indexes.size(); ++i) {
				largest_index = largest_index + in.output_indexes[i];
			}
			auto &lit = largest_indices[in.amount];
			if (largest_index > lit)
				lit = largest_index;
		}
		input_index++;
	}
	Height max_height = 0;
	for (auto lit : largest_indices) {
		UnlockTimePublickKeyHeightSpent unp;
		if (!read_amount_output(lit.first, lit.second, &unp))
			return false;
		max_height = std::max(max_height, unp.height);
	}
	*block_height = max_height;
	return true;
}

void BlockChainState::remove_from_pool(Hash tid) {
	auto tit = m_memory_state_tx.find(tid);
	if (tit == m_memory_state_tx.end())
		return;
	bool all_erased       = true;
	const Transaction &tx = tit->second.tx;
	for (const auto &input : tx.inputs) {
		if (input.type() == typeid(InputKey)) {
			const InputKey &in = boost::get<InputKey>(input);
			if (m_memory_state_ki_tx.erase(in.key_image) != 1)
				all_erased = false;
		}
	}
	const size_t my_size         = tit->second.binary_tx.size();
	const Amount my_fee_per_byte = tit->second.fee_per_byte();
	if (m_memory_state_fee_tx.erase(std::make_pair(my_fee_per_byte, tid)) != 1)
		all_erased = false;
	//	if (m_memory_state_fee_tx[my_fee_per_byte].empty())
	//		m_memory_state_fee_tx.erase(my_fee_per_byte);
	m_memory_state_total_size -= my_size;
	m_memory_state_tx.erase(tit);
	invariant(all_erased, "remove_memory_pool failed to erase everything");
	// We do not increment m_tx_pool_version, because removing tx from pool is
	// always followed by reset or increment
	auto min_size = m_memory_state_fee_tx.empty()
	                    ? 0
	                    : m_memory_state_tx.at(m_memory_state_fee_tx.begin()->second).binary_tx.size();
	auto min_fee_per_byte = m_memory_state_fee_tx.empty() ? 0 : m_memory_state_fee_tx.begin()->first;
	m_log(logging::INFO) << "Removed transaction with hash=" << tid << " size=" << my_size << " current_pool_size=("
	                     << m_memory_state_total_size - min_size << "+" << min_size << ")=" << m_memory_state_total_size
	                     << " count=" << m_memory_state_tx.size() << " min fee/byte=" << min_fee_per_byte << std::endl;
}

// Called only on transactions which passed validate_semantic()
// if double spend, conflict_height is set to actual conflict height
// if wrong sig, conflict_height is set to newest referenced height found up to the point of wrong sig
// if output not found, conflict height is set to currency max_block_height
// if no error, conflict_height is set to newest referenced height, (for coinbase transaction to 0)

void BlockChainState::redo_transaction(uint8_t major_block_version, bool generating, const Transaction &transaction,
    DeltaState *delta_state, BlockGlobalIndices *global_indices, Hash *newest_referenced_bid, bool check_sigs) const {
	const bool check_outputs = check_sigs;
	Hash tx_prefix_hash;
	if (m_config.paranoid_checks || check_sigs)
		tx_prefix_hash = get_transaction_prefix_hash(transaction);
	DeltaState tx_delta(delta_state->get_block_height(), delta_state->get_block_timestamp(),
	    delta_state->get_block_median_timestamp(), delta_state);
	global_indices->resize(global_indices->size() + 1);
	auto &my_indices = global_indices->back();
	my_indices.reserve(transaction.outputs.size());

	Height newest_referenced_height = 0;
	std::vector<std::vector<PublicKey>> all_output_keys;  // For half-size sigs
	std::vector<KeyImage> all_keyimages;                  // For half-size sigs
	for (size_t input_index = 0; input_index != transaction.inputs.size(); ++input_index) {
		const auto &input = transaction.inputs.at(input_index);
		if (input.type() == typeid(InputKey)) {
			const InputKey &in = boost::get<InputKey>(input);

			if (m_config.paranoid_checks || check_sigs || check_outputs || newest_referenced_bid) {
				Height height = 0;
				if (tx_delta.read_keyimage(in.key_image, &height))
					throw ConsensusErrorOutputSpent("Output already spent", in.key_image, height);
				//				if (in.output_indexes.size() < m_currency.minimum_anonymity(major_block_version) + 1 &&
				//				    !m_currency.is_dust(in.amount)) {
				//					if (m_currency.net == "main")
				//						throw ConsensusError("Anonymity too low");
				// In test/stage net we lack enough coins of each non-dust denomination
				//				}
				std::vector<size_t> global_indexes;
				if (!relative_output_offsets_to_absolute(&global_indexes, in.output_indexes))
					throw ConsensusError("Output indexes invalid in input");
				std::vector<PublicKey> output_keys(global_indexes.size());
				for (size_t i = 0; i != global_indexes.size(); ++i) {
					UnlockTimePublickKeyHeightSpent unp;
					if (!tx_delta.read_amount_output(in.amount, global_indexes[i], &unp))
						throw ConsensusErrorOutputDoesNotExist("Output does not exist", input_index, global_indexes[i]);
					if (unp.auditable && global_indexes.size() != 1)
						throw ConsensusErrorBadOutputOrSignature("Auditable output mixed", unp.height);
					if (!m_currency.is_transaction_unlocked(major_block_version, unp.unlock_block_or_timestamp,
					        delta_state->get_block_height(), delta_state->get_block_timestamp(),
					        delta_state->get_block_median_timestamp()))
						throw ConsensusErrorBadOutputOrSignature("Output locked", unp.height);
					output_keys[i]           = unp.public_key;
					newest_referenced_height = std::max(newest_referenced_height, unp.height);
				}
				if (m_config.paranoid_checks || check_sigs) {
					if (transaction.signatures.type() == typeid(RingSignatures)) {
						auto &signatures = boost::get<RingSignatures>(transaction.signatures);
						//						std::vector<const PublicKey *> output_key_pointers;
						//						output_key_pointers.reserve(output_keys.size());
						//						std::for_each(output_keys.begin(), output_keys.end(),
						//									  [&output_key_pointers](const PublicKey &key) {
						// output_key_pointers.push_back(&key); });
						if (!check_ring_signature(tx_prefix_hash, in.key_image, output_keys.data(), output_keys.size(),
						        signatures.signatures.at(input_index),
						        delta_state->get_block_height() >= m_currency.key_image_subgroup_checking_height)) {
							throw ConsensusErrorBadOutputOrSignature{
							    "Bad signature or output reference changed", newest_referenced_height};
						}
					} else if (transaction.signatures.type() == typeid(RingSignature3)) {
						all_output_keys.push_back(std::move(output_keys));
						all_keyimages.push_back(in.key_image);
					} else
						throw ConsensusError("Unknown signatures type");
				}
			}
			tx_delta.store_keyimage(in.key_image, delta_state->get_block_height());
		}
	}
	if (!all_output_keys.empty()) {
		invariant(transaction.signatures.type() == typeid(RingSignature3), "");
		auto &signatures = boost::get<RingSignature3>(transaction.signatures);
		if (!crypto::check_ring_signature3(tx_prefix_hash, all_keyimages, all_output_keys, signatures))
			throw ConsensusErrorBadOutputOrSignature{
			    "Bad signature or output reference changed", newest_referenced_height};
	}
	if (newest_referenced_bid) {
		// get_chain cannot fail if got all corresponding output keys successfully
		invariant(get_chain(newest_referenced_height, newest_referenced_bid), "");
	}
	for (const auto &output : transaction.outputs) {
		if (output.type() == typeid(OutputKey)) {
			const auto &key_output = boost::get<OutputKey>(output);
			auto global_index = tx_delta.push_amount_output(key_output.amount, transaction.unlock_block_or_timestamp, 0,
			    key_output.public_key, key_output.is_auditable);  // DeltaState ignores unlock point
			my_indices.push_back(global_index);
		}
	}
	tx_delta.apply(delta_state);
	// delta_state might be memory pool, we protect it from half-modification
}

void BlockChainState::undo_transaction(IBlockChainState *delta_state, Height, const Transaction &tx) {
	for (auto oit = tx.outputs.rbegin(); oit != tx.outputs.rend(); ++oit) {
		if (oit->type() == typeid(OutputKey)) {
			const auto &key_output = boost::get<OutputKey>(*oit);
			delta_state->pop_amount_output(
			    key_output.amount, tx.unlock_block_or_timestamp, key_output.public_key, key_output.is_auditable);
		}
	}
	for (auto iit = tx.inputs.rbegin(); iit != tx.inputs.rend(); ++iit) {
		if (iit->type() == typeid(InputKey)) {
			const InputKey &in = boost::get<InputKey>(*iit);
			unprocess_input(in);
			delta_state->delete_keyimage(in.key_image);
		}
	}
}

void BlockChainState::redo_block(const Block &block,
    const api::BlockHeader &info,
    DeltaState *delta_state,
    BlockGlobalIndices *global_indices) const {
	redo_transaction(
	    block.header.major_version, true, block.header.base_transaction, delta_state, global_indices, nullptr, false);
	for (auto tit = block.transactions.begin(); tit != block.transactions.end(); ++tit) {
		redo_transaction(block.header.major_version, false, *tit, delta_state, global_indices, nullptr, false);
	}
}

void BlockChainState::redo_block(const Hash &bhash, const Block &block, const api::BlockHeader &info) {
	DeltaState delta(info.height, info.timestamp, info.timestamp_median, this);
	BlockGlobalIndices global_indices;
	global_indices.reserve(block.transactions.size() + 1);
	const bool check_sigs = m_config.paranoid_checks || !m_currency.is_in_hard_checkpoint_zone(info.height + 1);
	if (check_sigs)
		m_ring_checker.start_work(this, m_currency, block, info.height, info.timestamp, info.timestamp_median,
		    info.height >= m_currency.key_image_subgroup_checking_height);
	redo_block(block, info, &delta, &global_indices);
	if (check_sigs) {
		auto errors = m_ring_checker.move_errors();
		if (!errors.empty())
			throw errors.front();  // We report first error only
	}
	delta.apply(this);  // Will remove from pool by key_image
	for (auto tit = block.transactions.begin(); tit != block.transactions.end(); ++tit) {
		for (size_t input_index = 0; input_index != tit->inputs.size(); ++input_index)
			if (tit->inputs.at(input_index).type() == typeid(InputKey))
				process_input(block.header.transaction_hashes.at(tit - block.transactions.begin()), input_index,
				    boost::get<InputKey>(tit->inputs.at(input_index)));
	}
	m_tx_pool_version = 2;

	auto key =
	    BLOCK_GLOBAL_INDICES_PREFIX + DB::to_binary_key(bhash.data, sizeof(bhash.data)) + BLOCK_GLOBAL_INDICES_SUFFIX;
	BinaryArray ba = seria::to_binary(global_indices);
	m_db.put(key, ba, true);

	auto now = std::chrono::steady_clock::now();
	if (m_config.net != "main" ||
	    std::chrono::duration_cast<std::chrono::milliseconds>(now - m_log_redo_block_timestamp).count() > 1000) {
		m_log_redo_block_timestamp = now;
		m_log(logging::INFO) << "redo_block height=" << info.height << " bid=" << bhash
		                     << " #tx=" << block.transactions.size() << std::endl;
	} else {
		if (m_config.paranoid_checks || check_sigs)  // No point in writing log before checkpoints
			m_log(logging::TRACE) << "redo_block height=" << info.height << " bid=" << bhash
			                      << " #tx=" << block.transactions.size() << std::endl;
	}
}

void BlockChainState::undo_block(const Hash &bhash, const Block &block, Height height) {
	auto now = std::chrono::steady_clock::now();
	if (m_config.net != "main" ||
	    std::chrono::duration_cast<std::chrono::milliseconds>(now - m_log_redo_block_timestamp).count() > 1000) {
		m_log_redo_block_timestamp = now;
		m_log(logging::INFO) << "undo_block height=" << height << " bid=" << bhash
		                     << " new tip_bid=" << block.header.previous_block_hash << std::endl;
	} else {
		if (m_config.paranoid_checks)
			m_log(logging::TRACE) << "undo_block height=" << height << " bid=" << bhash
			                      << " new tip_bid=" << block.header.previous_block_hash << std::endl;
	}
	for (auto tit = block.transactions.rbegin(); tit != block.transactions.rend(); ++tit) {
		undo_transaction(this, height, *tit);
	}
	undo_transaction(this, height, block.header.base_transaction);

	auto key =
	    BLOCK_GLOBAL_INDICES_PREFIX + DB::to_binary_key(bhash.data, sizeof(bhash.data)) + BLOCK_GLOBAL_INDICES_SUFFIX;
	m_db.del(key, true);
}

bool BlockChainState::read_block_output_global_indices(const Hash &bid, BlockGlobalIndices *indices) const {
	BinaryArray rb;
	auto key =
	    BLOCK_GLOBAL_INDICES_PREFIX + DB::to_binary_key(bid.data, sizeof(bid.data)) + BLOCK_GLOBAL_INDICES_SUFFIX;
	if (!m_db.get(key, rb))
		return false;
	seria::from_binary(*indices, rb);
	return true;
}

std::vector<api::Output> BlockChainState::get_random_outputs(uint8_t block_major_version, Amount amount,
    size_t output_count, Height confirmed_height, Timestamp block_timestamp, Timestamp block_median_timestamp) const {
	std::vector<api::Output> result;
	std::vector<api::Output> spent_result;
	size_t total_count = next_global_index_for_amount(amount);
	// We might need better algorithm if we have lots of locked amounts
	std::set<size_t> tried_or_added;
	crypto::random_engine<uint64_t> generator;
	std::lognormal_distribution<double> distribution(1.9, 1.0);  // Magic params here
	const uint32_t linear_part = 150;                            // Magic params here
	size_t attempts            = 0;
	for (; result.size() < output_count && attempts < output_count * 20; ++attempts) {  // TODO - 20
		size_t num = 0;
		if (result.size() % 2 == 0) {  // Half of outputs linear
			if (total_count <= linear_part)
				num = crypto::rand<size_t>() % total_count;  // 0 handled above
			else
				num = total_count - 1 - crypto::rand<size_t>() % linear_part;
		} else {
			double sample = distribution(generator);
			int d_num     = static_cast<int>(std::floor(total_count * (1 - std::pow(10, -sample / 10))));
			if (d_num < 0 || d_num >= int(total_count))
				continue;
			num = static_cast<size_t>(d_num);
		}
		if (!tried_or_added.insert(num).second)
			continue;
		UnlockTimePublickKeyHeightSpent unp;
		invariant(read_amount_output(amount, num, &unp), "num < total_count not found");
		if (unp.height > confirmed_height) {
			if (confirmed_height + 128 < get_tip_height())
				total_count = num;
			// heuristic - if confirmed_height is deep, the area under ditribution curve
			// with height < confirmed_height might be very small, so we adjust total_count
			// to get descent results after small number of attempts
			continue;
		}
		if (unp.auditable)
			continue;
		if (!m_currency.is_transaction_unlocked(block_major_version, unp.unlock_block_or_timestamp, confirmed_height,
		        block_timestamp, block_median_timestamp))
			continue;
		if (unp.spent && spent_result.size() >= output_count)
			continue;  // We need only so much spent
		api::Output item;
		item.amount                    = amount;
		item.index                     = num;
		item.unlock_block_or_timestamp = unp.unlock_block_or_timestamp;
		item.public_key                = unp.public_key;
		item.height                    = unp.height;
		(unp.spent ? spent_result : result).push_back(item);
	}
	if (result.size() < output_count) {
		// Read the whole index.
		size_t attempts = 0;
		for (DB::Cursor cur = m_db.rbegin(AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount));
		     result.size() < output_count && attempts < 10000 && !cur.end(); cur.next(), ++attempts) {  // TODO - 10000
			const char *be        = cur.get_suffix().data();
			const char *en        = be + cur.get_suffix().size();
			uint32_t global_index = common::integer_cast<uint32_t>(common::read_varint_sqlite4(be, en));
			if (tried_or_added.count(global_index) != 0)
				continue;
			UnlockTimePublickKeyHeightSpent unp;
			seria::from_binary(unp, cur.get_value_array());
			if (unp.auditable || unp.height > confirmed_height)
				continue;
			if (!m_currency.is_transaction_unlocked(block_major_version, unp.unlock_block_or_timestamp,
			        confirmed_height, block_timestamp, block_median_timestamp))
				continue;
			if (unp.spent && spent_result.size() >= output_count)
				continue;  // We need only so much spent
			api::Output item;
			item.amount                    = amount;
			item.index                     = global_index;
			item.unlock_block_or_timestamp = unp.unlock_block_or_timestamp;
			item.public_key                = unp.public_key;
			item.height                    = unp.height;
			(unp.spent ? spent_result : result).push_back(item);
		}
		// To satisfy minimum anonymity requirement for very rare coins, we add spent as a last resort
		while (result.size() < output_count && !spent_result.empty()) {
			result.push_back(spent_result.back());
			spent_result.pop_back();
		}
	}
	return result;
}

void BlockChainState::store_keyimage(const KeyImage &key_image, Height height) {
	auto key = KEYIMAGE_PREFIX + DB::to_binary_key(key_image.data, sizeof(key_image.data));
	m_db.put(key, seria::to_binary(height), true);
	auto tit = m_memory_state_ki_tx.find(key_image);
	if (tit == m_memory_state_ki_tx.end())
		return;
	remove_from_pool(tit->second);
}

void BlockChainState::delete_keyimage(const KeyImage &key_image) {
	auto key = KEYIMAGE_PREFIX + DB::to_binary_key(key_image.data, sizeof(key_image.data));
	m_db.del(key, true);
}

bool BlockChainState::read_keyimage(const KeyImage &key_image, Height *height) const {
	auto key = KEYIMAGE_PREFIX + DB::to_binary_key(key_image.data, sizeof(key_image.data));
	BinaryArray rb;
	if (!m_db.get(key, rb))
		return false;
	seria::from_binary(*height, rb);
	return true;
}

size_t BlockChainState::push_amount_output(
    Amount amount, BlockOrTimestamp unlock_time, Height block_height, const PublicKey &pk, bool is_auditable) {
	auto my_gi = next_global_index_for_amount(amount);
	auto key   = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount) + common::write_varint_sqlite4(my_gi);
	BinaryArray ba =
	    seria::to_binary(UnlockTimePublickKeyHeightSpent{unlock_time, pk, block_height, is_auditable, false, {}});
	m_db.put(key, ba, true);
	m_next_gi_for_amount[amount] += 1;
	return my_gi;
}

void BlockChainState::pop_amount_output(
    Amount amount, BlockOrTimestamp unlock_time, const PublicKey &pk, bool is_auditable) {
	auto next_gi = next_global_index_for_amount(amount);
	invariant(next_gi != 0, "BlockChainState::pop_amount_output underflow");
	next_gi -= 1;
	m_next_gi_for_amount[amount] -= 1;
	auto key = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount) + common::write_varint_sqlite4(next_gi);

	UnlockTimePublickKeyHeightSpent unp;
	invariant(read_amount_output(amount, next_gi, &unp), "BlockChainState::pop_amount_output element does not exist");
	invariant(!unp.spent && unp.unlock_block_or_timestamp == unlock_time && unp.public_key == pk &&
	              unp.auditable == is_auditable,
	    "BlockChainState::pop_amount_output popping wrong element");
	m_db.del(key, true);
}

size_t BlockChainState::next_global_index_for_amount(Amount amount) const {
	auto it = m_next_gi_for_amount.find(amount);
	if (it != m_next_gi_for_amount.end())
		return it->second;
	std::string prefix = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount);
	DB::Cursor cur2    = m_db.rbegin(prefix);
	size_t alt_in = cur2.end() ? 0 : common::integer_cast<size_t>(common::read_varint_sqlite4(cur2.get_suffix())) + 1;
	m_next_gi_for_amount[amount] = alt_in;
	return alt_in;
}

bool BlockChainState::read_amount_output(
    Amount amount, size_t global_index, UnlockTimePublickKeyHeightSpent *unp) const {
	auto key = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount) + common::write_varint_sqlite4(global_index);
	BinaryArray rb;
	if (!m_db.get(key, rb))
		return false;
	seria::from_binary(*unp, rb);
	return true;
}

void BlockChainState::process_input(const Hash &tid, size_t iid, const InputKey &input) {
	if (chain_reaction == 0)
		return;
	if (input.output_indexes.size() == 1) {
		UnlockTimePublickKeyHeightSpent unp;
		invariant(read_amount_output(input.amount, input.output_indexes.at(0), &unp), "");
		spend_output(
		    std::move(unp), input.amount, input.output_indexes.at(0), std::numeric_limits<size_t>::max(), 0, true);
		return;
	}
	if (chain_reaction == 1)
		return;
	const auto input_index = m_next_nz_input_index;
	auto din_key           = DIN_PREFIX + common::write_varint_sqlite4(m_next_nz_input_index);
	m_next_nz_input_index += 1;
	std::vector<size_t> global_indexes;
	invariant(relative_output_offsets_to_absolute(&global_indexes, input.output_indexes), "");
	InputDesc din;
	std::vector<UnlockTimePublickKeyHeightSpent> unspents;
	for (size_t i = 0; i != global_indexes.size(); ++i) {
		const auto global_index = global_indexes.at(i);
		UnlockTimePublickKeyHeightSpent unp;
		invariant(read_amount_output(input.amount, global_index, &unp), "");
		if (unp.spent)
			continue;
		din.first.push_back(global_index);
		unspents.push_back(std::move(unp));
	}
	if (din.first.size() > 1)
		for (size_t i = 0; i != din.first.size(); ++i) {
			unspents[i].dins.push_back(input_index);
			auto key = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(input.amount) +
			           common::write_varint_sqlite4(din.first[i]);
			m_db.put(key, seria::to_binary(unspents[i]), false);
		}
	m_db.put(din_key, seria::to_binary(din), true);
	if (din.first.size() == 1) {
		spend_output(std::move(unspents[0]), input.amount, din.first[0], input_index, 0, true);
	}
}

void BlockChainState::unprocess_input(const InputKey &input) {
	if (chain_reaction == 0)
		return;
	if (input.output_indexes.size() == 1) {
		UnlockTimePublickKeyHeightSpent unp;
		invariant(read_amount_output(input.amount, input.output_indexes.at(0), &unp), "");
		spend_output(
		    std::move(unp), input.amount, input.output_indexes.at(0), std::numeric_limits<size_t>::max(), 0, false);
		return;
	}
	if (chain_reaction == 1)
		return;
	m_next_nz_input_index -= 1;
	const auto input_index = m_next_nz_input_index;
	auto din_key           = DIN_PREFIX + common::write_varint_sqlite4(input_index);
	BinaryArray din_ba;
	invariant(m_db.get(din_key, din_ba), "");
	InputDesc din;
	seria::from_binary(din, din_ba);
	invariant(din.second.empty(), "");
	if (din.first.size() > 1)
		for (auto global_index : din.first) {
			UnlockTimePublickKeyHeightSpent unp;
			invariant(read_amount_output(input.amount, global_index, &unp), "");
			invariant(!unp.dins.empty() && unp.dins.back() == input_index, "");
			unp.dins.pop_back();
			auto key = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(input.amount) +
			           common::write_varint_sqlite4(global_index);
			m_db.put(key, seria::to_binary(unp), false);
		}
	m_db.del(din_key, true);
	if (din.first.size() == 1) {
		UnlockTimePublickKeyHeightSpent unp;
		invariant(read_amount_output(input.amount, din.first[0], &unp), "");
		spend_output(std::move(unp), input.amount, din.first[0], input_index, 0, false);
	}
}

void BlockChainState::spend_output(UnlockTimePublickKeyHeightSpent &&output, Amount amount, size_t global_index,
    size_t trigger_input_index, size_t level, bool spent) {
	if (level > 2)
		std::cout << "Sure spent level=" << level << " am:gi=" << amount << ":" << global_index << std::endl;
	auto key = AMOUNT_OUTPUT_PREFIX + common::write_varint_sqlite4(amount) + common::write_varint_sqlite4(global_index);
	bool no_subgroup_check_aftermath =
	    (amount == 6299999999000000 && global_index == 0) || (amount == 18899999999000000 && global_index == 0);
	if (spent) {
		invariant(no_subgroup_check_aftermath || output.spent == 0, "");
		output.spent += 1;
	} else {
		invariant(no_subgroup_check_aftermath || output.spent == 1, "");
		output.spent -= 1;
	}
	m_db.put(key, seria::to_binary(output), false);
	if (spent && output.spent > 1)
		return;
	if (!spent && output.spent > 0)
		return;
	if (!spent)  // Not the fastest code, but undo is rare
		std::reverse(output.dins.begin(), output.dins.end());
	for (auto input_index : output.dins) {
		if (input_index == trigger_input_index)
			continue;
		auto din_key = DIN_PREFIX + common::write_varint_sqlite4(input_index);
		BinaryArray din_ba;
		invariant(m_db.get(din_key, din_ba), "");
		InputDesc din;
		seria::from_binary(din, din_ba);
		size_t only_index = std::numeric_limits<size_t>::max();
		if (spent) {
			size_t found_index = std::lower_bound(din.first.begin(), din.first.end(), global_index) - din.first.begin();
			invariant(found_index != din.first.size() && din.first.at(found_index) == global_index, "");
			din.first.erase(din.first.begin() + found_index);
			din.second.push_back(global_index);
			//			std::cout << "Removing spent " << amount << ":" << global_index << " from " << din.tid << ":" <<
			// din.index << std::endl;
			if (din.first.size() == 1) {
				only_index = din.first.back();
			}
		} else {
			if (din.first.size() == 1) {
				only_index = din.first.back();
			}
			invariant(!din.second.empty() && din.second.back() == global_index, "");
			din.second.pop_back();
			size_t insert_index =
			    std::lower_bound(din.first.begin(), din.first.end(), global_index) - din.first.begin();
			din.first.insert(din.first.begin() + insert_index, global_index);
		}
		m_db.put(din_key, seria::to_binary(din), false);
		if (only_index != std::numeric_limits<size_t>::max()) {
			UnlockTimePublickKeyHeightSpent unp;
			invariant(read_amount_output(amount, only_index, &unp), "");
			spend_output(std::move(unp), amount, only_index, input_index, level + 1, spent);
		}
	}
}

void BlockChainState::test_print_outputs() {
	Amount previous_amount   = (Amount)-1;
	size_t next_global_index = 0;
	int total_counter        = 0;
	std::map<Amount, size_t> coins;
	for (DB::Cursor cur = m_db.begin(AMOUNT_OUTPUT_PREFIX); !cur.end(); cur.next()) {
		const char *be      = cur.get_suffix().data();
		const char *en      = be + cur.get_suffix().size();
		auto amount         = common::read_varint_sqlite4(be, en);
		size_t global_index = common::integer_cast<size_t>(common::read_varint_sqlite4(be, en));
		if (be != en)
			std::cout << "Excess value bytes for amount=" << amount << " index=" << global_index << std::endl;
		if (amount != previous_amount) {
			if (previous_amount != (Amount)-1) {
				if (!coins.insert(std::make_pair(previous_amount, next_global_index)).second) {
					std::cout << "Duplicate amount for previous_amount=" << previous_amount
					          << " next_global_index=" << next_global_index << std::endl;
				}
			}
			previous_amount   = amount;
			next_global_index = 0;
		}
		if (global_index != next_global_index) {
			std::cout << "Bad output index for amount=" << amount << " index=" << global_index << std::endl;
		}
		next_global_index += 1;
		if (++total_counter % 2000000 == 0)
			std::cout << "Working on amount=" << amount << " index=" << global_index << std::endl;
	}
	total_counter = 0;
	std::cout << "Total coins=" << total_counter << " total stacks=" << coins.size() << std::endl;
	for (auto &&co : coins) {
		auto total_count = next_global_index_for_amount(co.first);
		if (total_count != co.second)
			std::cout << "Wrong next_global_index_for_amount amount=" << co.first << " total_count=" << total_count
			          << " should be " << co.second << std::endl;
		for (size_t i = 0; i != total_count; ++i) {
			UnlockTimePublickKeyHeightSpent unp;
			if (!read_amount_output(co.first, i, &unp))
				std::cout << "Failed to read amount=" << co.first << " index=" << i << std::endl;
			if (++total_counter % 1000000 == 0)
				std::cout << "Working on amount=" << co.first << " index=" << i << std::endl;
		}
	}
}
