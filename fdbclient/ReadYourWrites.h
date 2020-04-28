/*
 * ReadYourWrites.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_READYOURWRITES_H
#define FDBCLIENT_READYOURWRITES_H
#include "FDBTypes.h"
#pragma once

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/RYWIterator.h"
#include <list>

//SOMEDAY: Optimize getKey to avoid using getRange

struct ReadYourWritesTransactionOptions {
	bool readYourWritesDisabled : 1;
	bool readAheadDisabled : 1;
	bool readSystemKeys : 1;
	bool writeSystemKeys : 1;
	bool nextWriteDisableConflictRange : 1;
	bool debugRetryLogging : 1;
	bool disableUsedDuringCommitProtection : 1;
	double timeoutInSeconds;
	int maxRetries;
	int snapshotRywEnabled;

	ReadYourWritesTransactionOptions() {}
	explicit ReadYourWritesTransactionOptions(Transaction const& tr);
	void reset(Transaction const& tr);
	bool getAndResetWriteConflictDisabled();
};

struct TransactionDebugInfo : public ReferenceCounted<TransactionDebugInfo> {
	std::string transactionName;
	double lastRetryLogTime;

	TransactionDebugInfo() : transactionName(""), lastRetryLogTime() { }
};

//Values returned by a ReadYourWritesTransaction will contain a reference to the transaction's arena. Therefore, keeping a reference to a value
//longer than its creating transaction would hold all of the memory generated by the transaction
class ReadYourWritesTransaction : NonCopyable, public ReferenceCounted<ReadYourWritesTransaction>, public FastAllocated<ReadYourWritesTransaction> {
public:
	static ReadYourWritesTransaction* allocateOnForeignThread() {
		ReadYourWritesTransaction *tr = (ReadYourWritesTransaction*)ReadYourWritesTransaction::operator new( sizeof(ReadYourWritesTransaction) );
		tr->tr.preinitializeOnForeignThread();
		return tr;
	}

	explicit ReadYourWritesTransaction( Database const& cx );
	~ReadYourWritesTransaction();

	void setVersion( Version v ) { tr.setVersion(v); }
	Future<Version> getReadVersion();
	Optional<Version> getCachedReadVersion() { return tr.getCachedReadVersion(); }
	Future< Optional<Value> > get( const Key& key, bool snapshot = false );
	Future< Key > getKey( const KeySelector& key, bool snapshot = false );
	Future< Standalone<RangeResultRef> > getRange( const KeySelector& begin, const KeySelector& end, int limit, bool snapshot = false, bool reverse = false );
	Future< Standalone<RangeResultRef> > getRange( KeySelector begin, KeySelector end, GetRangeLimits limits, bool snapshot = false, bool reverse = false );
	Future< Standalone<RangeResultRef> > getRange( const KeyRange& keys, int limit, bool snapshot = false, bool reverse = false ) {
		return getRange( KeySelector( firstGreaterOrEqual(keys.begin), keys.arena() ),
			KeySelector( firstGreaterOrEqual(keys.end), keys.arena() ), limit, snapshot, reverse );
	}
	Future< Standalone<RangeResultRef> > getRange( const KeyRange& keys, GetRangeLimits limits, bool snapshot = false, bool reverse = false ) {
		return getRange( KeySelector( firstGreaterOrEqual(keys.begin), keys.arena() ),
			KeySelector( firstGreaterOrEqual(keys.end), keys.arena() ), limits, snapshot, reverse );
	}

	[[nodiscard]] Future<Standalone<VectorRef<const char*>>> getAddressesForKey(const Key& key);
	Future<int64_t> getEstimatedRangeSizeBytes( const KeyRangeRef& keys );

	void addReadConflictRange( KeyRangeRef const& keys );
	void makeSelfConflicting() { tr.makeSelfConflicting(); }

	void atomicOp( const KeyRef& key, const ValueRef& operand, uint32_t operationType );
	void set( const KeyRef& key, const ValueRef& value );
	void clear( const KeyRangeRef& range );
	void clear( const KeyRef& key );

	[[nodiscard]] Future<Void> watch(const Key& key);

	void addWriteConflictRange( KeyRangeRef const& keys );

	[[nodiscard]] Future<Void> commit();
	Version getCommittedVersion() { return tr.getCommittedVersion(); }
	int64_t getApproximateSize() { return approximateSize; }
	[[nodiscard]] Future<Standalone<StringRef>> getVersionstamp();

	void setOption( FDBTransactionOptions::Option option, Optional<StringRef> value = Optional<StringRef>() );

	[[nodiscard]] Future<Void> onError(Error const& e);

	// These are to permit use as state variables in actors:
	ReadYourWritesTransaction() : cache(&arena), writes(&arena) {}
	void operator=(ReadYourWritesTransaction&& r) BOOST_NOEXCEPT;
	ReadYourWritesTransaction(ReadYourWritesTransaction&& r) BOOST_NOEXCEPT;

	virtual void addref() { ReferenceCounted<ReadYourWritesTransaction>::addref(); }
	virtual void delref() { ReferenceCounted<ReadYourWritesTransaction>::delref(); }

	void cancel();
	void reset();
	void debugTransaction(UID dID) { tr.debugTransaction(dID); }

	Future<Void> debug_onIdle() {  return reading; }

	// Used by ThreadSafeTransaction for exceptions thrown in void methods
	Error deferredError;

	void checkDeferredError() { tr.checkDeferredError(); if (deferredError.code() != invalid_error_code) throw deferredError; }

	void getWriteConflicts( KeyRangeMap<bool> *result );

	Database getDatabase() const {
		return tr.getDatabase();
	}

	const TransactionInfo& getTransactionInfo() const {
		return tr.info;
	}

	Standalone<RangeResultRef> getReadConflictRangeIntersecting(KeyRangeRef kr);
	Standalone<RangeResultRef> getWriteConflictRangeIntersecting(KeyRangeRef kr);

private:
	friend class RYWImpl;

	Arena arena;
	Transaction tr;
	SnapshotCache cache;
	WriteMap writes;
	CoalescedKeyRefRangeMap<bool> readConflicts;
	Map<Key, std::vector<Reference<Watch>>> watchMap;                      // Keys that are being watched in this transaction
	Promise<Void> resetPromise;
	AndFuture reading;
	int retries;
	int64_t approximateSize;
	Future<Void> timeoutActor;
	double creationTime;
	bool commitStarted;

	Reference<TransactionDebugInfo> transactionDebugInfo;

	void resetTimeout();
	void updateConflictMap( KeyRef const& key, WriteMap::iterator& it ); // pre: it.segmentContains(key)
	void updateConflictMap( KeyRangeRef const& keys, WriteMap::iterator& it ); // pre: it.segmentContains(keys.begin), keys are already inside this->arena
	void writeRangeToNativeTransaction(KeyRangeRef const& keys);

	void resetRyow(); // doesn't reset the encapsulated transaction, or creation time/retry state
	KeyRef getMaxReadKey();
	KeyRef getMaxWriteKey();

	bool checkUsedDuringCommit();

	void debugLogRetries(Optional<Error> error = Optional<Error>());

	void setOptionImpl( FDBTransactionOptions::Option option, Optional<StringRef> value = Optional<StringRef>() );
	void applyPersistentOptions();

	std::vector<std::pair<FDBTransactionOptions::Option, Optional<Standalone<StringRef>>>> persistentOptions;
	ReadYourWritesTransactionOptions options;
};

#endif
