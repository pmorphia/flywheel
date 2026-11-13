// The same workload as particles.fw, for comparison: c++ -O3 -o particles-cpp particles.cpp
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
struct Particle {
    double x, y, vx, vy, life;
};
static int64_t seed = 12345;
static double next_random() {
    seed = (seed * 1103515245 + 12345) % 2147483648LL;
    return static_cast<double>(seed) / 2147483648.0;
}
int main(int argc, char **argv) {
    long count = argc > 1 ? std::atol(argv[1]) : 50000, frames = argc > 2 ? std::atol(argv[2]) : 600;
    std::vector<Particle> particles;
    for (long i = 0; i < count; ++i) {
        double x = next_random() * 1280, y = next_random() * 720, vx = next_random() * 200 - 100,
               vy = next_random() * 200 - 100, life = 1 + next_random() * 4;
        particles.push_back({x, y, vx, vy, life});
    }
    double dt = 1.0 / 60.0;
    auto began = std::chrono::steady_clock::now();
    for (long frame = 0; frame < frames; ++frame)
        for (size_t i = 0; i < particles.size(); ++i) {
            Particle p = particles[i];
            double vx = p.vx, vy = p.vy + 300 * dt, x = p.x + vx * dt, y = p.y + vy * dt;
            if (x < 0) {
                x = -x;
                vx = -vx;
            } else if (x > 1280) {
                x = 2560 - x;
                vx = -vx;
            }
            if (y > 720) {
                y = 1440 - y;
                vy = -vy * 0.9;
            }
            double life = p.life - dt;
            if (life <= 0)
                life += 5;
            particles[i] = {x, y, vx, vy, life};
        }
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    double sum = 0;
    for (auto &p : particles)
        sum += p.x + p.y + p.life;
    std::printf("checksum %.3f\n", sum);
    std::printf("%ld particles, %ld frames: %.3f ms per frame\n", count, frames,
                elapsed * 1000 / static_cast<double>(frames));
}
