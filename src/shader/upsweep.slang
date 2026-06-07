import constants;

StructuredBuffer<uint> elementCounts : register(t0, space0);
RWStructuredBuffer<uint> globalHistogram : register(u1, space0);
RWStructuredBuffer<uint> partitionHistogram : register(u2, space0);
StructuredBuffer<uint> keys : register(t3, space0);

groupshared uint localHistogram[RADIX];

[shader("compute")]
[numthreads(WORKGROUP_SIZE)]
void main(uint3 groupThreadID: SV_GroupThreadID, uint3 groupId: SV_GroupID, uniform int pass,
          uniform uint maxElementCount) {
  uint elementCount = min(elementCounts[0], maxElementCount);

  uint index = groupThreadID.x;
  uint partitionIndex = groupId.x;
  uint partitionStart = partitionIndex * PARTITION_SIZE;

  // This condition is uniform across the workgroup, keeping every barrier converged.
  if (partitionStart < elementCount) {
    for (uint i = index; i < RADIX; i += WORKGROUP_SIZE) {
      localHistogram[i] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // local histogram
    for (int i = 0; i < PARTITION_DIVISION; ++i) {
      uint keyIndex = partitionStart + WORKGROUP_SIZE * i + index;
      uint key = keyIndex < elementCount ? keys[keyIndex] : 0xffffffff;
      uint radix = bitfieldExtract(key, 8 * pass, 8);
      __atomic_add(localHistogram[radix], 1, MemoryOrder.Relaxed);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i = index; i < RADIX; i += WORKGROUP_SIZE) {
      // set to partition histogram
      partitionHistogram[RADIX * partitionIndex + i] = localHistogram[i];

      // add to global histogram
      __atomic_add(globalHistogram[RADIX * pass + i], localHistogram[i], MemoryOrder.Relaxed);
    }
  }
}
