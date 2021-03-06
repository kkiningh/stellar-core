// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LocalNode.h"

#include "util/types.h"
#include "xdrpp/marshal.h"
#include "util/Logging.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include <algorithm>
#include "lib/json/json.h"

namespace stellar
{
using xdr::operator==;
using xdr::operator<;

LocalNode::LocalNode(SecretKey const& secretKey, bool isValidator,
                     SCPQuorumSet const& qSet, SCP* scp)
    : mNodeID(secretKey.getPublicKey())
    , mSecretKey(secretKey)
    , mIsValidator(isValidator)
    , mQSet(qSet)
    , mSCP(scp)
{
    adjustQSet(mQSet);
    mQSetHash = sha256(xdr::xdr_to_opaque(mQSet));

    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << PubKeyUtils::toShortString(mNodeID)
                      << " qSet: " << hexAbbrev(mQSetHash);

    mSingleQSet = std::make_shared<SCPQuorumSet>(buildSingletonQSet(mNodeID));
    gSingleQSetHash = sha256(xdr::xdr_to_opaque(*mSingleQSet));
}

SCPQuorumSet
LocalNode::buildSingletonQSet(NodeID const& nodeID)
{
    SCPQuorumSet qSet;
    qSet.threshold = 1;
    qSet.validators.emplace_back(nodeID);
    return qSet;
}

bool
LocalNode::isQuorumSetSaneInternal(NodeID const& nodeID,
                                   SCPQuorumSet const& qSet,
                                   std::set<NodeID>& knownNodes)
{
    auto& v = qSet.validators;
    auto& i = qSet.innerSets;

    size_t totEntries = v.size() + i.size();

    // threshold is within the proper range
    if (qSet.threshold >= 1 && qSet.threshold <= totEntries)
    {
        for (auto const& n : v)
        {
            auto r = knownNodes.insert(n);
            if (!r.second)
            {
                // n was already present
                return false;
            }
        }

        for (auto const& iSet : i)
        {
            if (!isQuorumSetSaneInternal(nodeID, iSet, knownNodes))
            {
                return false;
            }
        }

        return true;
    }
    else
    {
        return false;
    }
}

// helper function that:
//  * removes occurences of 'self'
//  * removes redundant inner sets (threshold = 0)
//     * empty {}
//     * reached because of self was { t: 1, self, other }
//  * simplifies singleton innersets
//      { t:1, { innerSet } } into innerSet

void
LocalNode::adjustQSetHelper(SCPQuorumSet& qSet)
{
    auto& v = qSet.validators;
    auto& i = qSet.innerSets;
    auto it = i.begin();
    while (it != i.end())
    {
        adjustQSetHelper(*it);
        // remove redundant sets
        // note: they may not be empty (threshold reached because of self)
        if (it->threshold == 0)
        {
            it = i.erase(it);
            if (qSet.threshold)
            {
                qSet.threshold--;
            }
        }
        else
        {
            it++;
        }
    }

    // removes self from validators
    auto itv = v.begin();
    while (itv != v.end())
    {
        if (*itv == mNodeID)
        {
            if (qSet.threshold)
            {
                qSet.threshold--;
            }
            itv = v.erase(itv);
        }
        else
        {
            itv++;
        }
    }

    // simplify quorum set if needed
    if (qSet.threshold == 1 && v.size() == 0 && i.size() == 1)
    {
        auto t = qSet.innerSets.back();
        qSet = t;
    }
}

void
LocalNode::adjustQSet(SCPQuorumSet& qSet)
{
    // transforms the qSet passed in into
    // { t: 2, self, { aQSet } }
    // where, newQset is the qSet obtained by deleting self

    auto aQSet = qSet;
    adjustQSetHelper(aQSet);

    qSet.threshold = 1;
    qSet.validators.clear();
    qSet.innerSets.clear();
    qSet.validators.emplace_back(mNodeID);

    if (aQSet.threshold != 0)
    {
        qSet.threshold++;
        qSet.innerSets.emplace_back(aQSet);
    }
}

bool
LocalNode::isQuorumSetSane(NodeID const& nodeID, SCPQuorumSet const& qSet)
{
    std::set<NodeID> allValidators;
    bool wellFormed = isQuorumSetSaneInternal(nodeID, qSet, allValidators);
    // it's OK for a non validating node to not have itself in its quorum set
    return wellFormed && ((allValidators.find(nodeID) != allValidators.end()) ||
                          (!mIsValidator && nodeID == mNodeID));
}

void
LocalNode::updateQuorumSet(SCPQuorumSet const& qSet)
{
    mQSetHash = sha256(xdr::xdr_to_opaque(qSet));
    mQSet = qSet;
}

SCPQuorumSet const&
LocalNode::getQuorumSet()
{
    return mQSet;
}

Hash const&
LocalNode::getQuorumSetHash()
{
    return mQSetHash;
}

SecretKey const&
LocalNode::getSecretKey()
{
    return mSecretKey;
}

SCPQuorumSetPtr
LocalNode::getSingletonQSet(NodeID const& nodeID)
{
    return std::make_shared<SCPQuorumSet>(buildSingletonQSet(nodeID));
}
void
LocalNode::forAllNodesInternal(SCPQuorumSet const& qset,
                               std::function<void(NodeID const&)> proc)
{
    for (auto const& n : qset.validators)
    {
        proc(n);
    }
    for (auto const& q : qset.innerSets)
    {
        forAllNodesInternal(q, proc);
    }
}

// runs proc over all nodes contained in qset
void
LocalNode::forAllNodes(SCPQuorumSet const& qset,
                       std::function<void(NodeID const&)> proc)
{
    std::set<NodeID> done;
    forAllNodesInternal(qset, [&](NodeID const& n)
                        {
                            auto ins = done.insert(n);
                            if (ins.second)
                            {
                                proc(n);
                            }
                        });
}

// if a validator is repeated multiple times its weight is only the
// weight of the first occurrence
uint64
LocalNode::getNodeWeight(NodeID const& nodeID, SCPQuorumSet const& qset)
{
    uint64 n = qset.threshold;
    uint64 d = qset.innerSets.size() + qset.validators.size();
    uint64 res;

    for (auto const& qsetNode : qset.validators)
    {
        if (qsetNode == nodeID)
        {
            bigDivide(res, UINT64_MAX, n, d);
            return res;
        }
    }

    for (auto const& q : qset.innerSets)
    {
        uint64 leafW = getNodeWeight(nodeID, q);
        if (leafW)
        {
            bigDivide(res, leafW, n, d);
            return res;
        }
    }

    return 0;
}

bool
LocalNode::isQuorumSliceInternal(SCPQuorumSet const& qset,
                                 std::vector<NodeID> const& nodeSet)
{
    uint32 thresholdLeft = qset.threshold;
    for (auto const& validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if (it != nodeSet.end())
        {
            thresholdLeft--;
            if (thresholdLeft <= 0)
            {
                return true;
            }
        }
    }

    for (auto const& inner : qset.innerSets)
    {
        if (isQuorumSliceInternal(inner, nodeSet))
        {
            thresholdLeft--;
            if (thresholdLeft <= 0)
            {
                return true;
            }
        }
    }
    return false;
}

bool
LocalNode::isQuorumSlice(SCPQuorumSet const& qSet,
                         std::vector<NodeID> const& nodeSet)
{
    CLOG(TRACE, "SCP") << "LocalNode::isQuorumSlice"
                       << " nodeSet.size: " << nodeSet.size();

    return isQuorumSliceInternal(qSet, nodeSet);
}

// called recursively
bool
LocalNode::isVBlockingInternal(SCPQuorumSet const& qset,
                               std::vector<NodeID> const& nodeSet)
{
    // There is no v-blocking set for {\empty}
    if (qset.threshold == 0)
    {
        return false;
    }

    int leftTillBlock =
        (int)((1 + qset.validators.size() + qset.innerSets.size()) -
              qset.threshold);

    for (auto const& validator : qset.validators)
    {
        auto it = std::find(nodeSet.begin(), nodeSet.end(), validator);
        if (it != nodeSet.end())
        {
            leftTillBlock--;
            if (leftTillBlock <= 0)
            {
                return true;
            }
        }
    }
    for (auto const& inner : qset.innerSets)
    {
        if (isVBlockingInternal(inner, nodeSet))
        {
            leftTillBlock--;
            if (leftTillBlock <= 0)
            {
                return true;
            }
        }
    }

    return false;
}

bool
LocalNode::isVBlocking(SCPQuorumSet const& qSet,
                       std::vector<NodeID> const& nodeSet)
{
    CLOG(TRACE, "SCP") << "LocalNode::isVBlocking"
                       << " nodeSet.size: " << nodeSet.size();

    return isVBlockingInternal(qSet, nodeSet);
}

bool
LocalNode::isVBlocking(SCPQuorumSet const& qSet,
                       std::map<NodeID, SCPEnvelope> const& map,
                       std::function<bool(SCPStatement const&)> const& filter)
{
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second.statement))
        {
            pNodes.push_back(it.first);
        }
    }

    return isVBlocking(qSet, pNodes);
}

bool
LocalNode::isQuorum(
    SCPQuorumSet const& qSet, std::map<NodeID, SCPEnvelope> const& map,
    std::function<SCPQuorumSetPtr(SCPStatement const&)> const& qfun,
    std::function<bool(SCPStatement const&)> const& filter)
{
    std::vector<NodeID> pNodes;
    for (auto const& it : map)
    {
        if (filter(it.second.statement))
        {
            pNodes.push_back(it.first);
        }
    }

    size_t count = 0;
    do
    {
        count = pNodes.size();
        std::vector<NodeID> fNodes(pNodes.size());
        auto quorumFilter = [&](NodeID nodeID) -> bool
        {
            return isQuorumSlice(*qfun(map.find(nodeID)->second.statement),
                                 pNodes);
        };
        auto it = std::copy_if(pNodes.begin(), pNodes.end(), fNodes.begin(),
                               quorumFilter);
        fNodes.resize(std::distance(fNodes.begin(), it));
        pNodes = fNodes;
    } while (count != pNodes.size());

    return isQuorumSlice(qSet, pNodes);
}

std::vector<NodeID>
LocalNode::findClosestVBlocking(
    SCPQuorumSet const& qset, std::map<NodeID, SCPEnvelope> const& map,
    std::function<bool(SCPStatement const&)> const& filter)
{
    std::set<NodeID> s;
    for (auto const& n : map)
    {
        if (filter(n.second.statement))
        {
            s.emplace(n.first);
        }
    }
    return findClosestVBlocking(qset, s);
}

std::vector<NodeID>
LocalNode::findClosestVBlocking(SCPQuorumSet const& qset,
                                std::set<NodeID> const& nodes)
{
    size_t leftTillBlock =
        ((1 + qset.validators.size() + qset.innerSets.size()) - qset.threshold);

    std::vector<NodeID> res;

    // first, compute how many top level items need to be blocked
    for (auto const& validator : qset.validators)
    {
        auto it = nodes.find(validator);
        if (it == nodes.end())
        {
            leftTillBlock--;
            if (leftTillBlock == 0)
            {
                // already blocked
                return std::vector<NodeID>();
            }
        }
        else
        {
            // save this for later
            res.emplace_back(validator);
        }
    }

    struct orderBySize
    {
        bool operator()(std::vector<NodeID> const& v1,
                        std::vector<NodeID> const& v2)
        {
            return v1.size() < v2.size();
        }
    };

    std::multiset<std::vector<NodeID>, orderBySize> resInternals;

    for (auto const& inner : qset.innerSets)
    {
        auto v = findClosestVBlocking(inner, nodes);
        if (v.size() == 0)
        {
            leftTillBlock--;
            if (leftTillBlock == 0)
            {
                // already blocked
                return std::vector<NodeID>();
            }
        }
        else
        {
            resInternals.emplace(v);
        }
    }

    // use the top level validators to get closer
    if (res.size() > leftTillBlock)
    {
        res.resize(leftTillBlock);
    }
    leftTillBlock -= res.size();

    // use subsets to get closer, using the smallest ones first
    auto it = resInternals.begin();
    while (leftTillBlock != 0 && it != resInternals.end())
    {
        res.insert(res.end(), it->begin(), it->end());
        leftTillBlock--;
        it++;
    }

    return res;
}

void
LocalNode::toJson(SCPQuorumSet const& qSet, Json::Value& value) const
{
    value["t"] = qSet.threshold;
    auto& entries = value["v"];
    for (auto const& v : qSet.validators)
    {
        entries.append(mSCP->getDriver().toShortString(v));
    }
    for (auto const& s : qSet.innerSets)
    {
        Json::Value iV;
        toJson(s, iV);
        entries.append(iV);
    }
}

std::string
LocalNode::to_string(SCPQuorumSet const& qSet) const
{
    Json::Value v;
    toJson(qSet, v);
    Json::FastWriter fw;
    return fw.write(v);
}

NodeID const&
LocalNode::getNodeID()
{
    return mNodeID;
}

bool
LocalNode::isValidator()
{
    return mIsValidator;
}
}
