// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#pragma once

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "BlockChainState.hpp"
#include "CryptoNote.hpp"
#include "Wallet.hpp"
#include "platform/DB.hpp"
#include "rpc_api.hpp"

namespace cn {

class Config;

class IWalletState {
public:
	virtual ~IWalletState() {}

	virtual Amount add_incoming_output(const api::Output &, const Hash &tid)    = 0;  // added amount may be lower
	virtual Amount add_incoming_keyimage(Height block_height, const KeyImage &) = 0;
	virtual Amount add_incoming_deterministic_input(Height block_height, Amount am, size_t gi, const PublicKey &pk) = 0;
	virtual void add_transaction(
	    Height block_height, const Hash &tid, const TransactionPrefix &tx, const api::Transaction &ptx) = 0;
};

class WalletStateBasic : protected IWalletState {
public:
	typedef platform::DB DB;

	explicit WalletStateBasic(
	    logging::ILogger &, const Config &, const Currency &, const std::string &cache_name, bool is_det_viewonly);
	const Currency &get_currency() const { return m_currency; };

	Hash get_tip_bid() const { return m_tip.hash; }
	Height get_tip_height() const { return m_tip_height; }
	const api::BlockHeader &get_tip() const { return m_tip; }
	bool read_chain(Height, api::BlockHeader &) const;

	std::vector<Hash> get_sparse_chain() const;

	// methods used by API
	bool api_add_unspent(std::vector<api::Output> *result, Amount *total_amount, const std::string &address,
	    Height confirmed_height, Amount max_amount = std::numeric_limits<Amount>::max()) const;
	std::vector<api::Block> api_get_transfers(const std::string &address, Height *from_height, Height *to_height,
	    bool forward, size_t desired_tx_count = std::numeric_limits<size_t>::max()) const;
	virtual std::vector<api::Output> api_get_locked_or_unconfirmed_unspent(
	    const std::string &address, Height height) const;
	virtual api::Balance get_balance(const std::string &address, Height height) const;
	std::vector<api::Transfer> api_get_unlocked_transfers(
	    const std::string &address, Height from_height, Height to_height = std::numeric_limits<Height>::max()) const;
	bool get_transaction(Hash tid, TransactionPrefix *tx, api::Transaction *ptx) const;
	bool has_transaction(Hash tid) const;

	struct UndoValue {
		bool exists = false;
		common::BinaryArray value;
	};
	struct HeightAmounGi {
		Height height       = 0;
		Amount amount       = 0;
		size_t global_index = 0;
	};

	void db_commit();
	void test_undo_blocks();
	void test_print_everything(const std::string &str);
	static void combine_balance(api::Balance &balance, const api::Output &output, int locked_op, int spendable_op);

	const Config &get_config() const { return m_config; }

protected:
	const Hash m_genesis_bid;
	const Config &m_config;
	const Currency &m_currency;
	logging::LoggerRef m_log;

	DB m_db;

	void push_chain(const api::BlockHeader &);
	void pop_chain();
	bool empty_chain() const { return m_tip_height + 1 == m_tail_height; }
	void reset_chain(Height new_tail_height);
	Height get_tail_height() const { return m_tail_height; }
	void fix_empty_chain();  // push genesis block
	api::BlockHeader read_chain(Height) const;

	bool is_memory_spent(const api::Output &output) const {
		return get_mempool_kis_or_pks().count(output.key_image) != 0;
	}
	virtual const std::map<crypto::EllipticCurvePoint, int> &get_mempool_kis_or_pks() const;
	virtual void on_first_transaction_found(Timestamp ts) {}
	void unlock(Height now_height, Timestamp now);

	bool read_from_unspent_index(const HeightAmounGi &value, api::Output *) const;
	bool read_by_keyimage(const crypto::EllipticCurvePoint &ki, HeightAmounGi *value) const;

	bool try_add_incoming_output(const api::Output &, Amount *confirmed_balance_delta) const;
	bool try_adding_incoming_keyimage(const KeyImage &, api::Output *spending_output) const;
	bool try_adding_deterministic_input(Amount am, size_t gi, api::Output *spending_output) const;
	// returns true if our keyimage

	// methods to add incoming tx
	Amount add_incoming_output(const api::Output &, const Hash &tid) override;  // added amount may be lower
	Amount add_incoming_keyimage(Height block_height, const KeyImage &) override;
	Amount add_incoming_deterministic_input(Height block_height, Amount am, size_t gi, const PublicKey &pk) override;
	void add_transaction(Height, const Hash &tid, const TransactionPrefix &tx, const api::Transaction &ptx) override;

	std::string format_output(const api::Output &output);

private:
	bool try_adding_incoming_ki_or_pk(const crypto::EllipticCurvePoint &, api::Output *spending_output) const;
	Amount add_incoming_ki_or_pk(Height block_height, const crypto::EllipticCurvePoint &);

	Height m_tip_height  = -1;
	Height m_tail_height = 0;
	api::BlockHeader m_tip;

	const bool is_det_viewonly;
	void put_am_gi_he(Amount am, size_t gi, Height he, const PublicKey &pk);
	bool get_am_gi_he(Amount am, size_t gi, Height *he, PublicKey *pk) const;
	// DB generic undo machinery
	typedef std::map<std::string, UndoValue> UndoMap;
	UndoMap current_undo_map;
	UndoMap::iterator record_undo(UndoMap &undo_map, const std::string &key);
	void put_with_undo(const std::string &key, const common::BinaryArray &value, bool nooverwrite);
	void del_with_undo(const std::string &key, bool mustexist);
	void save_db_state(Height state, const UndoMap &undo_map);
	void undo_db_state(Height state);

	// indices implemenation
	Amount add_incoming_output(const api::Output &, const Hash &tid, bool just_unlocked);
	void modify_balance(const api::Output &output, int locked_op, int spendable_op);
	// lock/unlock
	void add_to_lock_index(const api::Output &, const Hash &tid);
	void remove_from_lock_index(const api::Output &);

	void unlock(Height now_height, api::Output &&output);
	void read_unlock_index(std::map<std::pair<Amount, size_t>, api::Output> *add, const std::string &index_prefix,
	    const std::string &address, BlockOrTimestamp begin, BlockOrTimestamp end) const;
	std::map<std::pair<Amount, size_t>, api::Output> get_unlocked_outputs(
	    const std::string &address, Height from_height, Height to_height) const;

	// add coin/spend coin
	void add_to_unspent_index(const api::Output &);
	void remove_from_unspent_index(const api::Output &);
	bool for_each_in_unspent_index(
	    const std::string &address, Height from, Height to, std::function<bool(api::Output &&)> fun) const;
	void update_keyimage(const crypto::EllipticCurvePoint &m, const HeightAmounGi &value, bool nooverwrite);
};

}  // namespace cn

namespace seria {
void ser_members(cn::WalletStateBasic::HeightAmounGi &v, ISeria &s);
void ser_members(cn::WalletStateBasic::UndoValue &v, seria::ISeria &s);
}  // namespace seria
