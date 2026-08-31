#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace heliosim {

struct BarnesHutStats {
    std::size_t nodeVisits = 0;
    std::size_t directInteractions = 0;
    std::size_t approximatedNodes = 0;
};

// A three-dimensional Barnes-Hut octree. The tree stores indices into the
// supplied arrays, which must therefore outlive the tree.
class BarnesHutTree {
public:
    BarnesHutTree(const std::vector<glm::dvec3>& positions,
                  const std::vector<double>& masses,
                  std::size_t leafCapacity = 8,
                  unsigned int maxDepth = 64)
        : positions_(positions),
          masses_(masses),
          leafCapacity_(std::max<std::size_t>(leafCapacity, 1)),
          maxDepth_(maxDepth) {
        if (positions_.size() != masses_.size()) {
            throw std::invalid_argument("BarnesHutTree positions and masses must have equal sizes");
        }
        if (positions_.empty()) return;

        glm::dvec3 minimum = positions_.front();
        glm::dvec3 maximum = positions_.front();
        for (const glm::dvec3& position : positions_) {
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }

        const glm::dvec3 extent = maximum - minimum;
        double halfSize = 0.5 * std::max({extent.x, extent.y, extent.z});
        halfSize = std::max(halfSize, 1e-12);
        // Keep bodies on the maximum face strictly inside the root cube.
        halfSize = std::nextafter(halfSize, std::numeric_limits<double>::infinity());

        nodes_.reserve(positions_.size() * 2);
        nodes_.emplace_back((minimum + maximum) * 0.5, halfSize);
        root_ = 0;
        for (std::size_t body = 0; body < positions_.size(); ++body) {
            insert(root_, body, 0);
        }
        for (Node& node : nodes_) {
            if (node.mass != 0.0) {
                node.centerOfMass = node.weightedPosition / node.mass;
            } else {
                node.centerOfMass = node.center;
            }
        }
    }

    void computeAccelerations(std::vector<glm::dvec3>& accelerations,
                              double forceScale,
                              double theta = 0.5,
                              double softeningSquared = 1e-6,
                              BarnesHutStats* stats = nullptr) const {
        accelerations.assign(positions_.size(), glm::dvec3(0.0));
        if (stats) *stats = {};
        if (root_ == noNode) return;

        const double safeSoftening = std::max(softeningSquared, 0.0);
        for (std::size_t target = 0; target < positions_.size(); ++target) {
            evaluateNode(root_, target, true, forceScale, theta, safeSoftening,
                         accelerations[target], stats);
        }
    }

    std::size_t nodeCount() const { return nodes_.size(); }

private:
    static constexpr std::size_t noNode = std::numeric_limits<std::size_t>::max();

    struct Node {
        explicit Node(const glm::dvec3& nodeCenter, double nodeHalfSize)
            : center(nodeCenter), halfSize(nodeHalfSize) {
            children.fill(noNode);
        }

        glm::dvec3 center;
        double halfSize;
        double mass = 0.0;
        glm::dvec3 weightedPosition{0.0};
        glm::dvec3 centerOfMass{0.0};
        std::array<std::size_t, 8> children;
        std::vector<std::size_t> bodies;
        bool leaf = true;
    };

    unsigned int octantFor(const Node& node, const glm::dvec3& position) const {
        unsigned int octant = 0;
        if (position.x >= node.center.x) octant |= 1;
        if (position.y >= node.center.y) octant |= 2;
        if (position.z >= node.center.z) octant |= 4;
        return octant;
    }

    std::size_t childFor(std::size_t nodeIndex, std::size_t body) {
        const unsigned int octant = octantFor(nodes_[nodeIndex], positions_[body]);
        if (nodes_[nodeIndex].children[octant] != noNode) {
            return nodes_[nodeIndex].children[octant];
        }

        const glm::dvec3 parentCenter = nodes_[nodeIndex].center;
        const double childHalfSize = nodes_[nodeIndex].halfSize * 0.5;
        const glm::dvec3 offset(
            (octant & 1) ? childHalfSize : -childHalfSize,
            (octant & 2) ? childHalfSize : -childHalfSize,
            (octant & 4) ? childHalfSize : -childHalfSize);
        const std::size_t childIndex = nodes_.size();
        nodes_.emplace_back(parentCenter + offset, childHalfSize);
        nodes_[nodeIndex].children[octant] = childIndex;
        return childIndex;
    }

    void insert(std::size_t nodeIndex, std::size_t body, unsigned int depth) {
        const double bodyMass = masses_[body];
        nodes_[nodeIndex].mass += bodyMass;
        nodes_[nodeIndex].weightedPosition += positions_[body] * bodyMass;

        if (nodes_[nodeIndex].leaf) {
            if (nodes_[nodeIndex].bodies.size() < leafCapacity_ || depth >= maxDepth_) {
                nodes_[nodeIndex].bodies.push_back(body);
                return;
            }

            std::vector<std::size_t> existingBodies =
                std::move(nodes_[nodeIndex].bodies);
            nodes_[nodeIndex].bodies.clear();
            nodes_[nodeIndex].leaf = false;
            for (std::size_t existingBody : existingBodies) {
                const std::size_t child = childFor(nodeIndex, existingBody);
                insert(child, existingBody, depth + 1);
            }
        }

        const std::size_t child = childFor(nodeIndex, body);
        insert(child, body, depth + 1);
    }

    static void addAcceleration(const glm::dvec3& displacement,
                                double sourceMass,
                                double forceScale,
                                double softeningSquared,
                                glm::dvec3& acceleration) {
        const double distanceSquared = glm::dot(displacement, displacement) + softeningSquared;
        if (distanceSquared <= 0.0) return;
        const double inverseDistance = 1.0 / std::sqrt(distanceSquared);
        acceleration += displacement *
            (forceScale * sourceMass * inverseDistance / distanceSquared);
    }

    void evaluateNode(std::size_t nodeIndex,
                      std::size_t target,
                      bool containsTarget,
                      double forceScale,
                      double theta,
                      double softeningSquared,
                      glm::dvec3& acceleration,
                      BarnesHutStats* stats) const {
        const Node& node = nodes_[nodeIndex];
        if (stats) ++stats->nodeVisits;

        if (!containsTarget && node.mass != 0.0 && theta > 0.0) {
            const glm::dvec3 displacement = node.centerOfMass - positions_[target];
            const double distanceSquared = glm::dot(displacement, displacement);
            const double size = node.halfSize * 2.0;
            if (distanceSquared > 0.0 &&
                size * size < theta * theta * distanceSquared) {
                addAcceleration(displacement, node.mass, forceScale,
                                softeningSquared, acceleration);
                if (stats) ++stats->approximatedNodes;
                return;
            }
        }

        // A sufficiently distant leaf can use the same center-of-mass
        // approximation as an internal node. Only leaves near the target need
        // to fall back to their individual body interactions.
        if (node.leaf) {
            for (std::size_t source : node.bodies) {
                if (source == target || masses_[source] == 0.0) continue;
                addAcceleration(positions_[source] - positions_[target], masses_[source],
                                forceScale, softeningSquared, acceleration);
                if (stats) ++stats->directInteractions;
            }
            return;
        }

        const unsigned int targetOctant = octantFor(node, positions_[target]);
        for (unsigned int octant = 0; octant < node.children.size(); ++octant) {
            const std::size_t child = node.children[octant];
            if (child == noNode) continue;
            evaluateNode(child, target, containsTarget && octant == targetOctant,
                         forceScale, theta, softeningSquared, acceleration, stats);
        }
    }

    const std::vector<glm::dvec3>& positions_;
    const std::vector<double>& masses_;
    std::size_t leafCapacity_;
    unsigned int maxDepth_;
    std::vector<Node> nodes_;
    std::size_t root_ = noNode;
};

} // namespace heliosim
