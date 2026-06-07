import constants;

StructuredBuffer<uint> elementCounts : register(t0, space0);
RWStructuredBuffer<uint> globalHistogram : register(u1, space0);
RWStructuredBuffer<uint> partitionHistogram : register(u2, space0);

groupshared uint intermediate[WORKGROUP_SIZE];

// Each subgroup scans one radix; the host dispatches RADIX / waveCount workgroups.
[shader("compute")]
[numthreads(WORKGROUP_SIZE)]
void main(uint3 groupThreadID: SV_GroupThreadID, uint3 groupId: SV_GroupID,
          uint groupIndex: SV_GroupIndex, uniform int pass, uniform uint maxElementCount) {
  uint elementCount = min(elementCounts[0], maxElementCount);

  uint laneIndex = WaveGetLaneIndex();  // 0..31
  uint laneCount = WaveGetLaneCount();  // 32
  uint waveIndex = groupIndex / laneCount;
  uint waveCount = WORKGROUP_SIZE / laneCount;
  uint index = groupIndex;
  uint radix = groupId.x * waveCount + waveIndex;

  uint partitionCount = elementCount / PARTITION_SIZE + uint(elementCount % PARTITION_SIZE != 0);

  if (radix < RADIX) {
    uint reduction = 0;
    for (uint partitionBase = 0; partitionBase < partitionCount; partitionBase += laneCount) {
      uint partitionIndex = partitionBase + laneIndex;
      uint value =
          partitionIndex < partitionCount ? partitionHistogram[RADIX * partitionIndex + radix] : 0;
      uint excl = WavePrefixSum(value) + reduction;
      reduction += WaveActiveSum(value);
      if (partitionIndex < partitionCount) {
        partitionHistogram[RADIX * partitionIndex + radix] = excl;
      }
    }
  }

  if (groupId.x == 0) {
    GroupMemoryBarrierWithGroupSync();
    // one workgroup is responsible for global histogram prefix sum
    if (index < RADIX) {
      uint value = globalHistogram[RADIX * pass + index];
      uint excl = WavePrefixSum(value);
      uint sum = WaveActiveSum(value);

      if (WaveIsFirstLane()) {
        intermediate[waveIndex] = sum;
      }
      GroupMemoryBarrierWithGroupSync();

      if (index < RADIX / laneCount) {
        uint excl = WavePrefixSum(intermediate[index]);
        intermediate[index] = excl;
      }
      GroupMemoryBarrierWithGroupSync();

      excl += intermediate[waveIndex];
      globalHistogram[RADIX * pass + index] = excl;
    }
  }
}
