#pragma once
class Entity;
class Chunk;

// std::binary_function is deprecated (removed in C++17) and only ever supplied
// argument/result typedefs that nothing here reads - std::sort just needs operator().
class DistanceChunkSorter
{
private:
	double ix, iy, iz;

public:
    DistanceChunkSorter(shared_ptr<Entity> player);
	bool operator()(const Chunk *a, const Chunk *b) const;
};