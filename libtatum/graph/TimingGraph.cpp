#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "tatum_assert.hpp"
#include "tatum_error.hpp"
#include "TimingGraph.hpp"

namespace tatum {

//Builds a mapping from old to new ids by skipping values marked invalid
template<typename Id>
tatum::util::linear_map<Id,Id> compress_ids(const tatum::util::linear_map<Id,Id>& ids) {
    tatum::util::linear_map<Id,Id> id_map(ids.size());
    size_t i = 0;
    for(auto id : ids) {
        if(id) {
            //Valid
            id_map.insert(id, Id(i));
            ++i;
        }
    }

    return id_map;
}

//Returns a vector based on 'values', which has had entries dropped and 
//re-ordered according according to 'id_map'.
//
// Each entry in id_map corresponds to the assoicated element in 'values'.
// The value of the id_map entry is the new ID of the entry in values.
//
// If it is an invalid ID, the element in values is dropped.
// Otherwise the element is moved to the new ID location.
template<typename Id, typename T>
tatum::util::linear_map<Id,T> clean_and_reorder_values(const tatum::util::linear_map<Id,T>& values, const tatum::util::linear_map<Id,Id>& id_map) {
    TATUM_ASSERT(values.size() == id_map.size());

    //Allocate space for the values that will not be dropped
    tatum::util::linear_map<Id,T> result;

    //Move over the valid entries to their new locations
    for(size_t cur_idx = 0; cur_idx < values.size(); ++cur_idx) {
        Id old_id = Id(cur_idx);

        Id new_id = id_map[old_id];
        if (new_id) {
            //There is a valid mapping
            result.insert(new_id, std::move(values[old_id]));
        }
    }

    return result;
}

//Returns the set of new valid Ids defined by 'id_map'
//TOOD: merge with clean_and_reorder_values
template<typename Id>
tatum::util::linear_map<Id,Id> clean_and_reorder_ids(const tatum::util::linear_map<Id,Id>& id_map) {
    //For IDs, the values are the new id's stored in the map

    //Allocate a new vector to store the values that have been not dropped
    tatum::util::linear_map<Id,Id> result;

    //Move over the valid entries to their new locations
    for(size_t cur_idx = 0; cur_idx < id_map.size(); ++cur_idx) {
        Id old_id = Id(cur_idx);

        Id new_id = id_map[old_id];
        if (new_id) {
            result.insert(new_id, new_id);
        }
    }

    return result;
}

template<typename Container, typename ValId>
Container update_valid_refs(const Container& values, const tatum::util::linear_map<ValId,ValId>& id_map) {
    Container updated;

    for(ValId orig_val : values) {
        if(orig_val) {
            //Original item valid

            ValId new_val = id_map[orig_val];
            if(new_val) {
                //The original item exists in the new mapping
                updated.emplace_back(new_val);
            }
        }
    }
    return updated;
}

//Updates the Ids in 'values' based on id_map, even if the original or new mapping is not valid
template<typename Container, typename ValId>
Container update_all_refs(const Container& values, const tatum::util::linear_map<ValId,ValId>& id_map) {
    Container updated;

    for(ValId orig_val : values) {
        //The original item was valid
        ValId new_val = id_map[orig_val]; 
        //The original item exists in the new mapping
        updated.emplace_back(new_val);
    }

    return updated;
}




NodeId TimingGraph::add_node(const NodeType type) {

    //Reserve an ID
    NodeId node_id = NodeId(node_ids_.size());
    node_ids_.push_back(node_id);

    //Type
    node_types_.push_back(type);

    //Edges
    node_out_edges_.push_back(std::vector<EdgeId>());
    node_in_edges_.push_back(std::vector<EdgeId>());

    //Verify sizes
    TATUM_ASSERT(node_types_.size() == node_out_edges_.size());
    TATUM_ASSERT(node_types_.size() == node_in_edges_.size());

    //Return the ID of the added node
    return node_id;
}

EdgeId TimingGraph::add_edge(const NodeId src_node, const NodeId sink_node) {
    //We require that the source/sink node must already be in the graph,
    //  so we can update them with thier edge references
    TATUM_ASSERT(valid_node_id(src_node));
    TATUM_ASSERT(valid_node_id(sink_node));

    //Reserve an edge ID
    EdgeId edge_id = EdgeId(edge_ids_.size());
    edge_ids_.push_back(edge_id);

    //Create the edgge
    edge_src_nodes_.push_back(src_node);
    edge_sink_nodes_.push_back(sink_node);

    //Verify
    TATUM_ASSERT(edge_sink_nodes_.size() == edge_src_nodes_.size());

    //Update the nodes the edge references
    node_out_edges_[src_node].push_back(edge_id);
    node_in_edges_[sink_node].push_back(edge_id);

    //Return the edge id of the added edge
    return edge_id;
}

void TimingGraph::levelize() {
    //Levelizes the timing graph
    //This over-writes any previous levelization if it exists.
    //
    //Also records primary outputs

    //Clear any previous levelization
    level_nodes_.clear();
    level_ids_.clear();
    primary_outputs_.clear();

    //Allocate space for the first level
    level_nodes_.resize(1);

    //Copy the number of input edges per-node
    //These will be decremented to know when all a node's upstream parents have been
    //placed in a previous level (indicating that the node goes in the current level)
    //
    //Also initialize the first level (nodes with no fanin)
    std::vector<int> node_fanin_remaining(nodes().size());
    for(NodeId node_id : nodes()) {
        size_t node_fanin = node_in_edges(node_id).size();
        node_fanin_remaining[size_t(node_id)] = node_fanin;

        if(node_fanin == 0) {
            //Add a primary input
            level_nodes_[LevelId(0)].push_back(node_id);
        }
    }

    //Walk the graph from primary inputs (no fanin) to generate a topological sort
    //
    //We inspect the output edges of each node and decrement the fanin count of the
    //target node.  Once the fanin count for a node reaches zero it can be added
    //to the current level.
    int level_idx = 0;
    level_ids_.emplace_back(level_idx);

    bool inserted_node_in_level = true;
    while(inserted_node_in_level) { //If nothing was inserted we are finished
        inserted_node_in_level = false;

        for(const NodeId node_id : level_nodes_[LevelId(level_idx)]) {
            //Inspect the fanout
            for(EdgeId edge_id : node_out_edges(node_id)) {
                NodeId sink_node = edge_sink_node(edge_id);

                //Decrement the fanin count
                TATUM_ASSERT(node_fanin_remaining[size_t(sink_node)] > 0);
                node_fanin_remaining[size_t(sink_node)]--;

                //Add to the next level if all fanin has been seen
                if(node_fanin_remaining[size_t(sink_node)] == 0) {
                    //Ensure there is space by allocating the next level if required
                    level_nodes_.resize(level_idx+2);

                    //Add the node
                    level_nodes_[LevelId(level_idx+1)].push_back(sink_node);

                    inserted_node_in_level = true;
                }
            }

            //Also track the primary outputs
            if(node_out_edges(node_id).size() == 0) {
                primary_outputs_.push_back(node_id);
            }
        }

        if(inserted_node_in_level) {
            level_idx++;
            level_ids_.emplace_back(level_idx);
        }
    }
}

void TimingGraph::remove_node(const NodeId node_id) {
    TATUM_ASSERT(valid_node_id(node_id));

    //Invalidate all the references
    for(EdgeId in_edge : node_in_edges(node_id)) {
        if(!in_edge) continue;

        remove_edge(in_edge);
    }

    for(EdgeId out_edge : node_out_edges(node_id)) {
        if(!out_edge) continue;

        remove_edge(out_edge);
    }

    //Mark the node as invalid
    node_ids_[node_id] = NodeId::INVALID();
}

void TimingGraph::remove_edge(const EdgeId edge_id) {
    TATUM_ASSERT(valid_edge_id(edge_id));

    //Invalidate the upstream node to edge references
    NodeId src_node = edge_src_node(edge_id);    
    auto iter_out = std::find(node_out_edges_[src_node].begin(), node_out_edges_[src_node].end(), edge_id);
    TATUM_ASSERT(iter_out != node_out_edges_[src_node].end());
    *iter_out = EdgeId::INVALID();

    //Invalidate the downstream node to edge references
    NodeId sink_node = edge_sink_node(edge_id);    
    auto iter_in = std::find(node_in_edges_[sink_node].begin(), node_in_edges_[sink_node].end(), edge_id);
    TATUM_ASSERT(iter_in != node_in_edges_[sink_node].end());
    *iter_in = EdgeId::INVALID();

    //Mark the edge invalid
    edge_ids_[edge_id] = EdgeId::INVALID();
}

GraphIdMaps TimingGraph::compress() {
    auto node_id_map = compress_ids(node_ids_);
    auto edge_id_map = compress_ids(edge_ids_);

    remap_nodes(node_id_map);
    remap_edges(edge_id_map);

    validate();

    return {node_id_map, edge_id_map};
}

bool TimingGraph::validate() {
    bool valid = true;
    valid &= validate_sizes();
    valid &= validate_values();
    valid &= validate_structure();

    return valid;
}

GraphIdMaps TimingGraph::optimize_layout() {
    auto node_id_map = optimize_node_layout();
    remap_nodes(node_id_map);

    auto edge_id_map = optimize_edge_layout();
    remap_edges(edge_id_map);

    levelize();

    return {node_id_map, edge_id_map};
}

tatum::util::linear_map<EdgeId,EdgeId> TimingGraph::optimize_edge_layout() const {
    //Make all edges in a level be contiguous in memory

    //Determine the edges driven by each level of the graph
    std::vector<std::vector<EdgeId>> edge_levels;
    for(LevelId level_id : levels()) {
        edge_levels.push_back(std::vector<EdgeId>());
        for(auto node_id : level_nodes(level_id)) {

            //We walk the nodes according to the input-edge order.
            //This is the same order used by the arrival-time traversal (which is responsible
            //for most of the analyzer run-time), so matching it's order exactly results in
            //better cache locality
            for(EdgeId edge_id : node_in_edges(node_id)) {

                //edge_id is driven by nodes in level level_idx
                edge_levels[size_t(level_id)].push_back(edge_id);
            }
        }
    }

    //Maps from from original to new edge id, used to update node to edge refs
    tatum::util::linear_map<EdgeId,EdgeId> orig_to_new_edge_id(edges().size());

    //Determine the new order
    size_t iedge = 0;
    for(auto& edge_level : edge_levels) {
        for(const EdgeId orig_edge_id : edge_level) {

            //Save the new edge id to update nodes
            orig_to_new_edge_id[orig_edge_id] = EdgeId(iedge);

            ++iedge;
        }
    }

    for(auto new_id : orig_to_new_edge_id) {
        TATUM_ASSERT(new_id);
    }
    TATUM_ASSERT(iedge == edges().size());

    return orig_to_new_edge_id;
}

tatum::util::linear_map<NodeId,NodeId> TimingGraph::optimize_node_layout() const {
    //Make all nodes in a level be contiguous in memory

    /*
     * Keep a map of the old and new node ids to update edges
     * and node levels later
     */
    tatum::util::linear_map<NodeId,NodeId> orig_to_new_node_id(nodes().size());

    //Determine the new order
    size_t inode = 0;
    for(const LevelId level_id : levels()) {
        for(const NodeId old_node_id : level_nodes(level_id)) {
            //Record the new node id
            orig_to_new_node_id[old_node_id] = NodeId(inode);
            ++inode;
        }
    }

    for(auto new_id : orig_to_new_node_id) {
        TATUM_ASSERT(new_id);
    }
    TATUM_ASSERT(inode == nodes().size());

    return orig_to_new_node_id;
}

void TimingGraph::remap_nodes(const tatum::util::linear_map<NodeId,NodeId>& node_id_map) {
    //Update values
    node_ids_ = clean_and_reorder_ids(node_id_map);
    node_types_ = clean_and_reorder_values(node_types_, node_id_map);
    node_in_edges_ = clean_and_reorder_values(node_in_edges_, node_id_map);
    node_out_edges_ = clean_and_reorder_values(node_out_edges_, node_id_map);

    //Update references
    edge_src_nodes_ = update_all_refs(edge_src_nodes_, node_id_map);
    edge_sink_nodes_ = update_all_refs(edge_sink_nodes_, node_id_map);
}

void TimingGraph::remap_edges(const tatum::util::linear_map<EdgeId,EdgeId>& edge_id_map) {

    //Update values
    edge_ids_ = clean_and_reorder_ids(edge_id_map);
    edge_sink_nodes_ = clean_and_reorder_values(edge_sink_nodes_, edge_id_map);
    edge_src_nodes_ = clean_and_reorder_values(edge_src_nodes_, edge_id_map);

    //Update cross-references
    for(auto& edges_ref : node_in_edges_) {
        edges_ref = update_valid_refs(edges_ref, edge_id_map);
    }
    for(auto& edges_ref : node_out_edges_) {
        edges_ref = update_valid_refs(edges_ref, edge_id_map);
    }
}

bool TimingGraph::valid_node_id(const NodeId node_id) {
    return size_t(node_id) < node_ids_.size();
}

bool TimingGraph::valid_edge_id(const EdgeId edge_id) {
    return size_t(edge_id) < edge_ids_.size();
}

bool TimingGraph::valid_level_id(const LevelId level_id) {
    return size_t(level_id) < level_ids_.size();
}

bool TimingGraph::validate_sizes() {
    if(   node_ids_.size() != node_types_.size()
       || node_ids_.size() != node_in_edges_.size()
       || node_ids_.size() != node_out_edges_.size()) {
        throw tatum::Error("Inconsistent node attribute sizes");
    }

    if(   edge_ids_.size() != edge_sink_nodes_.size()
       || edge_ids_.size() != edge_src_nodes_.size()) {
        throw tatum::Error("Inconsistent edge attribute sizes");
    }

    return true;
}

bool TimingGraph::validate_values() {

    for(NodeId node_id : nodes()) {
        if(!valid_node_id(node_id)) {
            throw tatum::Error("Invalid node id");
        }

        for(EdgeId edge_id : node_in_edges_[node_id]) {
            if(!valid_edge_id(edge_id)) {
                throw tatum::Error("Invalid node-in-edge reference");
            }

            //Check that the references are consistent
            if(edge_sink_nodes_[edge_id] != node_id) {
                throw tatum::Error("Mismatched edge-sink/node-in-edge reference");
            }
        }
        for(EdgeId edge_id : node_out_edges_[node_id]) {
            if(!valid_edge_id(edge_id)) {
                throw tatum::Error("Invalid node-out-edge reference");
            }

            //Check that the references are consistent
            if(edge_src_nodes_[edge_id] != node_id) {
                throw tatum::Error("Mismatched edge-src/node-out-edge reference");
            }
        }
    }
    for(EdgeId edge_id : edges()) {
        if(!valid_edge_id(edge_id)) {
            throw tatum::Error("Invalid edge id");
        }
        if(!valid_node_id(edge_src_nodes_[edge_id])) {
            throw tatum::Error("Invalid edge source node");
        }
        if(!valid_node_id(edge_sink_nodes_[edge_id])) {
            throw tatum::Error("Invalid edge sink node");
        }
    }

    //TODO: more checking

    return true;
}

bool TimingGraph::validate_structure() {
    //Verify that the timing graph connectivity is as expected

    for(NodeId src_node : nodes()) {

        NodeType src_type = node_type(src_node);
        
        for(EdgeId out_edge : node_out_edges(src_node)) {
            NodeId sink_node = edge_sink_node(out_edge);
            NodeType sink_type = node_type(sink_node);

            //Check type connectivity
            if(src_type == NodeType::SOURCE) {

                if(   sink_type != NodeType::IPIN
                   && sink_type != NodeType::CPIN
                   && sink_type != NodeType::SINK) {
                    throw tatum::Error("SOURCE nodes should only drive IPIN, CPIN or SINK nodes");
                }

            } else if (src_type == NodeType::SINK) {
                throw tatum::Error("SINK nodes should not have out-going edges");
            } else if (src_type == NodeType::IPIN) {
                if(sink_type != NodeType::OPIN) {
                    throw tatum::Error("IPIN nodes should only drive OPIN nodes");
                }
            } else if (src_type == NodeType::OPIN) {
                if(   sink_type != NodeType::IPIN
                   && sink_type != NodeType::SINK) {
                    throw tatum::Error("OPIN nodes should only drive IPIN or SINK nodes");
                }
            } else if (src_type == NodeType::CPIN) {
                if(   sink_type != NodeType::SOURCE
                   && sink_type != NodeType::SINK) {
                    throw tatum::Error("CPIN nodes should only drive SOURCE or SINK nodes");
                }
            } else {
                throw tatum::Error("Unrecognized node type");
            }

            //Check that sinks have non fanout
            if(!node_out_edges(sink_node).empty()) {
                throw tatum::Error("SINK node should have no out-going edges");
            }
        }
    }

    for(NodeId node : primary_inputs()) {
        if(!node_in_edges(node).empty()) {
            throw tatum::Error("Primary input nodes should have no incoming edges");
        }
    }

    for(NodeId node : primary_outputs()) {
        if(!node_out_edges(node).empty()) {
            throw tatum::Error("Primary output node should have no outgoing edges");
        }
    }

    return true;
}

//Stream output for NodeType
std::ostream& operator<<(std::ostream& os, const NodeType type) {
    if      (type == NodeType::SOURCE)              os << "SOURCE";
    else if (type == NodeType::SINK)                os << "SINK";
    else if (type == NodeType::IPIN)                os << "IPIN";
    else if (type == NodeType::OPIN)                os << "OPIN";
    else if (type == NodeType::CPIN)                os << "CPIN";
    else throw std::domain_error("Unrecognized NodeType");
    return os;
}

std::ostream& operator<<(std::ostream& os, NodeId node_id) {
    if(node_id == NodeId::INVALID()) {
        return os << "Node(INVALID)";
    } else {
        return os << "Node(" << size_t(node_id) << ")";
    }
}

std::ostream& operator<<(std::ostream& os, EdgeId edge_id) {
    if(edge_id == EdgeId::INVALID()) {
        return os << "Edge(INVALID)";
    } else {
        return os << "Edge(" << size_t(edge_id) << ")";
    }
}

std::ostream& operator<<(std::ostream& os, DomainId domain_id) {
    if(domain_id == DomainId::INVALID()) {
        return os << "Domain(INVALID)";
    } else {
        return os << "Domain(" << size_t(domain_id) << ")";
    }
}

std::ostream& operator<<(std::ostream& os, LevelId level_id) {
    if(level_id == LevelId::INVALID()) {
        return os << "Level(INVALID)";
    } else {
        return os << "Level(" << size_t(level_id) << ")";
    }
}

} //namepsace
