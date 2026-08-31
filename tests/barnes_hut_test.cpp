#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "barnes_hut.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<glm::dvec3> exactAccelerations(
        const std::vector<glm::dvec3>& positions,
        const std::vector<double>& masses,
        double softeningSquared) {
    std::vector<glm::dvec3> result(positions.size(), glm::dvec3(0.0));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        for (std::size_t j = 0; j < positions.size(); ++j) {
            if (i == j) continue;
            const glm::dvec3 displacement = positions[j] - positions[i];
            const double distanceSquared =
                glm::dot(displacement, displacement) + softeningSquared;
            if (distanceSquared <= 0.0) continue;
            result[i] += displacement *
                (masses[j] / (distanceSquared * std::sqrt(distanceSquared)));
        }
    }
    return result;
}

void testExactTraversal() {
    const std::vector<glm::dvec3> positions = {
        {-3.0, 0.5, 1.0}, {2.0, -1.0, 0.25}, {0.1, 4.0, -2.0},
        {8.0, 2.0, 3.0}, {-1.0, -5.0, 2.0}
    };
    const std::vector<double> masses = {2.0, 5.0, 1.5, 9.0, 3.0};
    constexpr double softeningSquared = 1e-6;

    heliosim::BarnesHutTree tree(positions, masses, 1);
    std::vector<glm::dvec3> actual;
    tree.computeAccelerations(actual, 1.0, 0.0, softeningSquared);
    const std::vector<glm::dvec3> expected =
        exactAccelerations(positions, masses, softeningSquared);

    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double scale = std::max(glm::length(expected[i]), 1.0);
        require(glm::length(actual[i] - expected[i]) <= 1e-12 * scale,
                "theta=0 traversal must equal the direct calculation");
    }
}

void testApproximationAccuracy() {
    std::mt19937_64 random(0xBADC0FFEEULL);
    std::uniform_real_distribution<double> coordinate(-100.0, 100.0);
    std::uniform_real_distribution<double> mass(0.25, 20.0);
    std::vector<glm::dvec3> positions;
    std::vector<double> masses;
    positions.reserve(512);
    masses.reserve(512);
    for (int i = 0; i < 512; ++i) {
        positions.emplace_back(coordinate(random), coordinate(random), coordinate(random));
        masses.push_back(mass(random));
    }

    const std::vector<glm::dvec3> expected = exactAccelerations(positions, masses, 1e-6);
    heliosim::BarnesHutTree tree(positions, masses);
    std::vector<glm::dvec3> actual;
    tree.computeAccelerations(actual, 1.0, 0.5, 1e-6);

    double squaredError = 0.0;
    double squaredSignal = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const glm::dvec3 error = actual[i] - expected[i];
        squaredError += glm::dot(error, error);
        squaredSignal += glm::dot(expected[i], expected[i]);
    }
    const double relativeRmsError = std::sqrt(squaredError / squaredSignal);
    require(relativeRmsError < 0.02,
            "theta=0.5 relative RMS acceleration error must stay below 2%");
}

void testCoincidentBodies() {
    std::vector<glm::dvec3> positions(96, glm::dvec3(0.0));
    std::vector<double> masses(96, 1.0);
    positions.emplace_back(10.0, 0.0, 0.0);
    masses.push_back(4.0);

    heliosim::BarnesHutTree tree(positions, masses, 8);
    std::vector<glm::dvec3> accelerations;
    tree.computeAccelerations(accelerations, 1.0, 0.5, 1e-6);
    for (const glm::dvec3& acceleration : accelerations) {
        require(std::isfinite(acceleration.x) && std::isfinite(acceleration.y) &&
                    std::isfinite(acceleration.z),
                "coincident bodies must produce finite accelerations");
    }
}

void testSubQuadraticWork() {
    std::mt19937_64 random(0x12345678ULL);
    std::uniform_real_distribution<double> coordinate(-1000.0, 1000.0);
    constexpr std::size_t bodyCount = 2048;
    std::vector<glm::dvec3> positions;
    std::vector<double> masses(bodyCount, 1.0);
    positions.reserve(bodyCount);
    for (std::size_t i = 0; i < bodyCount; ++i) {
        positions.emplace_back(coordinate(random), coordinate(random), coordinate(random));
    }

    heliosim::BarnesHutTree tree(positions, masses);
    heliosim::BarnesHutStats stats;
    std::vector<glm::dvec3> accelerations;
    tree.computeAccelerations(accelerations, 1.0, 0.5, 1e-6, &stats);

    const std::size_t evaluatedInteractions =
        stats.directInteractions + stats.approximatedNodes;
    const std::size_t directInteractions = bodyCount * (bodyCount - 1);
    require(evaluatedInteractions < directInteractions / 4,
            "Barnes-Hut traversal evaluated " + std::to_string(evaluatedInteractions) +
                " interactions versus " + std::to_string(directInteractions) +
                " for direct summation");
}

} // namespace

int main() {
    testExactTraversal();
    testApproximationAccuracy();
    testCoincidentBodies();
    testSubQuadraticWork();
    std::cout << "Barnes-Hut tests passed\n";
    return 0;
}
