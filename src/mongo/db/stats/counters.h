// counters.h
/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/counter.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/message.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/with_alignment.h"

namespace mongo {

/**
 * for storing operation counters
 * note: not thread safe.  ok with that for speed
 */
class OpCounters {
public:
    OpCounters();
    void gotInserts(int n);
    void gotInsert();
    void gotQuery();
    void gotUpdate();
    void gotDelete();
    void gotGetMore();
    void gotCommand();

    void gotOp(int op, bool isCommand);

    BSONObj getObj() const;

    // thse are used by snmp, and other things, do not remove
    const AtomicUInt32* getInsert() const {
        return &_insert;
    }
    const AtomicUInt32* getQuery() const {
        return &_query;
    }
    const AtomicUInt32* getUpdate() const {
        return &_update;
    }
    const AtomicUInt32* getDelete() const {
        return &_delete;
    }
    const AtomicUInt32* getGetMore() const {
        return &_getmore;
    }
    const AtomicUInt32* getCommand() const {
        return &_command;
    }

private:
    void _checkWrap();

    CacheAligned<AtomicUInt32> _insert;
    CacheAligned<AtomicUInt32> _query;
    CacheAligned<AtomicUInt32> _update;
    CacheAligned<AtomicUInt32> _delete;
    CacheAligned<AtomicUInt32> _getmore;
    CacheAligned<AtomicUInt32> _command;
};

extern OpCounters globalOpCounters;
extern OpCounters replOpCounters;

class NetworkCounter {
public:
    // Increment the counters for the number of bytes read directly off the wire
    void hitPhysicalIn(long long bytes);
    void hitPhysicalOut(long long bytes);

    // Increment the counters for the number of bytes passed out of the TransportLayer to the
    // server
    void hitLogicalIn(long long bytes);
    void hitLogicalOut(long long bytes);

    void append(BSONObjBuilder& b);

private:
    CacheAligned<AtomicInt64> _physicalBytesIn{0};
    CacheAligned<AtomicInt64> _physicalBytesOut{0};

    // These two counters are always incremented at the same time, so
    // we place them on the same cache line.
    struct Together {
        AtomicInt64 logicalBytesIn{0};
        AtomicInt64 requests{0};
    };
    CacheAligned<Together> _together{};
    static_assert(sizeof(decltype(_together)) <= stdx::hardware_constructive_interference_size,
                  "cache line spill");

    CacheAligned<AtomicInt64> _logicalBytesOut{0};
};

extern NetworkCounter networkCounter;

// Provide a memory usage limitation for the stage object.
class StageMemCounter {
public:
    StageMemCounter() {}
    ~StageMemCounter() {}

    void incCachedMemSize(const StageType& type, const size_t& size);
    void decCachedMemSize(const StageType& type, const size_t& size);

    void incMemObj(const StageType& type);
    void decMemObj(const StageType& type);

    bool chkCachedMemOversize(const size_t& cachedMemSize) const;
    int64_t getTotalMemSize() const {
        return _totalMem.get();
    };

    BSONObj getObj() const;

private:
    Counter64 _totalMem;

    struct StageTypeCounter {
        Counter64 _objectCount;
        Counter64 _memSize;
    };
    StageTypeCounter _stageMap[STAGE_INVALID];
};

extern StageMemCounter globalStageMemCounters;
}
