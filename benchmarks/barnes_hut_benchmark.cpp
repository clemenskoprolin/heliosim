#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define HELIOSIM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define HELIOSIM_EXPORT
#endif

#include "barnes_hut.hpp"

namespace {

constexpr double kTheta = 0.5;
constexpr double kSofteningSquared = 1e-6;
constexpr std::size_t kLeafCapacity = 8;

struct Fixture {
    std::vector<glm::dvec3> positions;
    std::vector<double> masses;
};

struct BarnesHutMeasurement {
    double medianMilliseconds = 0.0;
    double minimumMilliseconds = 0.0;
    std::size_t nodeCount = 0;
    heliosim::BarnesHutStats stats;
};

std::uint64_t nextRandom(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

double randomUnit(std::uint64_t& state) {
    return static_cast<double>(nextRandom(state) >> 11) *
           (1.0 / 9007199254740992.0);
}

Fixture makeFixture(std::size_t bodyCount) {
    Fixture fixture;
    fixture.positions.reserve(bodyCount);
    fixture.masses.reserve(bodyCount);

    std::uint64_t randomState = 0xD1B54A32D192ED03ULL;
    for (std::size_t body = 0; body < bodyCount; ++body) {
        fixture.positions.emplace_back(
            randomUnit(randomState) * 2000.0 - 1000.0,
            randomUnit(randomState) * 2000.0 - 1000.0,
            randomUnit(randomState) * 2000.0 - 1000.0);
        fixture.masses.push_back(0.5 + randomUnit(randomState));
    }
    return fixture;
}

double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2;
    if (samples.size() % 2 == 0) {
        return (samples[middle - 1] + samples[middle]) * 0.5;
    }
    return samples[middle];
}

volatile double checksumSink = 0.0;

BarnesHutMeasurement measureBarnesHut(std::size_t bodyCount, int repetitions) {
    const Fixture fixture = makeFixture(bodyCount);
    std::vector<glm::dvec3> accelerations;

    auto solve = [&]() {
        heliosim::BarnesHutTree tree(
            fixture.positions, fixture.masses, kLeafCapacity);
        tree.computeAccelerations(
            accelerations, 1.0, kTheta, kSofteningSquared);
    };

    solve(); // Warm caches and one-time runtime paths before sampling.

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        solve();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());

        if (!accelerations.empty()) {
            const std::size_t sampleIndex =
                static_cast<std::size_t>(repetition) % accelerations.size();
            checksumSink += accelerations[sampleIndex].x;
        }
    }

    BarnesHutMeasurement measurement;
    measurement.medianMilliseconds = median(samples);
    measurement.minimumMilliseconds =
        *std::min_element(samples.begin(), samples.end());

    // Collect structural work counters outside the timed samples.
    heliosim::BarnesHutTree instrumentedTree(
        fixture.positions, fixture.masses, kLeafCapacity);
    instrumentedTree.computeAccelerations(
        accelerations, 1.0, kTheta, kSofteningSquared, &measurement.stats);
    measurement.nodeCount = instrumentedTree.nodeCount();
    return measurement;
}

double measureDirect(std::size_t bodyCount, int repetitions) {
    const Fixture fixture = makeFixture(bodyCount);
    std::vector<glm::dvec3> accelerations(bodyCount);

    auto solve = [&]() {
        std::fill(accelerations.begin(), accelerations.end(), glm::dvec3(0.0));
        for (std::size_t target = 0; target < bodyCount; ++target) {
            for (std::size_t source = 0; source < bodyCount; ++source) {
                if (source == target) continue;
                const glm::dvec3 displacement =
                    fixture.positions[source] - fixture.positions[target];
                const double distanceSquared =
                    glm::dot(displacement, displacement) + kSofteningSquared;
                accelerations[target] += displacement *
                    (fixture.masses[source] /
                     (distanceSquared * std::sqrt(distanceSquared)));
            }
        }
    };

    solve();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        solve();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
        if (!accelerations.empty()) checksumSink += accelerations.front().y;
    }
    return median(samples);
}

BarnesHutMeasurement lastMeasurement;

int repetitionsFor(std::size_t bodyCount) {
    if (bodyCount <= 10000) return 7;
    if (bodyCount <= 50000) return 5;
    return 3;
}

} // namespace

extern "C" {

HELIOSIM_EXPORT double benchmarkBarnesHutMilliseconds(int bodyCount, int repetitions) {
    if (bodyCount <= 0 || repetitions <= 0) return 0.0;
    lastMeasurement = measureBarnesHut(
        static_cast<std::size_t>(bodyCount), repetitions);
    return lastMeasurement.medianMilliseconds;
}

HELIOSIM_EXPORT double benchmarkBarnesHutMinimumMilliseconds() {
    return lastMeasurement.minimumMilliseconds;
}

HELIOSIM_EXPORT double benchmarkBarnesHutNodeCount() {
    return static_cast<double>(lastMeasurement.nodeCount);
}

HELIOSIM_EXPORT double benchmarkBarnesHutDirectInteractions() {
    return static_cast<double>(lastMeasurement.stats.directInteractions);
}

HELIOSIM_EXPORT double benchmarkBarnesHutApproximatedNodes() {
    return static_cast<double>(lastMeasurement.stats.approximatedNodes);
}

HELIOSIM_EXPORT double benchmarkDirectMilliseconds(int bodyCount, int repetitions) {
    if (bodyCount <= 0 || repetitions <= 0) return 0.0;
    return measureDirect(static_cast<std::size_t>(bodyCount), repetitions);
}

} // extern "C"

#ifndef __EMSCRIPTEN__
int main() {
    const std::vector<std::size_t> bodyCounts = {
        1000, 2000, 5000, 10000, 20000, 50000, 100000
    };

    std::cout << "bodies,median_ms,min_ms,solves_per_second,"
                 "six_solve_frame_ceiling_hz,nodes,direct_interactions,"
                 "approximated_nodes\n";
    std::cout << std::fixed << std::setprecision(3);
    for (std::size_t bodyCount : bodyCounts) {
        const BarnesHutMeasurement measurement =
            measureBarnesHut(bodyCount, repetitionsFor(bodyCount));
        const double solvesPerSecond =
            1000.0 / measurement.medianMilliseconds;
        const double frameCeiling = solvesPerSecond / 6.0;
        std::cout << bodyCount << ','
                  << measurement.medianMilliseconds << ','
                  << measurement.minimumMilliseconds << ','
                  << solvesPerSecond << ','
                  << frameCeiling << ','
                  << measurement.nodeCount << ','
                  << measurement.stats.directInteractions << ','
                  << measurement.stats.approximatedNodes << '\n';
    }

    std::cout << "direct_1000_ms," << measureDirect(1000, 5) << '\n';
    std::cout << "direct_10000_ms," << measureDirect(10000, 3) << '\n';
    std::cout << "checksum," << checksumSink << '\n';
    return 0;
}
#endif
