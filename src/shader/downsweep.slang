import constants;

StructuredBuffer<uint> elementCounts : register(t0, space0);
RWStructuredBuffer<uint> globalHistogram : register(u1, space0);
RWStructuredBuffer<uint> partitionHistogram : register(u2, space0);
StructuredBuffer<uint> keysIn : register(t3, space0);
RWStructuredBuffer<uint> keysOut : register(u4, space0);
#ifdef KEY_VALUE
StructuredBuffer<uint> valuesIn : register(t5, space0);
RWStructuredBuffer<uint> valuesOut : register(u6, space0);
#endif  // KEY_VALUE

groupshared uint localHistogram[PARTITION_SIZE];  // (R, S=16)=4096, (P) for alias. take maximum.
groupshared uint localHistogramSum[RADIX];
groupshared uint subgroupSums[8];

// returns 0b00000....11111, where msb is laneIndex-1.
uint4 GetExclusiveWaveMask(uint laneIndex) {
    uint4 mask = uint4(0, 0, 0, 0);
    [ForceUnroll]
    for (int i = 0; i < 4; ++i) {
        if (laneIndex < 32) {
            mask[i] = (1u << laneIndex) - 1u;
            laneIndex = 0;
        } else {
            mask[i] = 0xFFFFFFFF;
            laneIndex -= 32;
        }
    }
    return mask;
}

uint GetBitCount(uint4 value) {
  uint4 result = countbits(value);
  return result[0] + result[1] + result[2] + result[3];
}

[shader("compute")]
[numthreads(WORKGROUP_SIZE)]
void main(uint3 groupThreadID: SV_GroupThreadID, uint3 groupId: SV_GroupID,
          uint groupIndex: SV_GroupIndex, uniform int pass, uniform uint maxElementCount) {
    uint elementCount = min(elementCounts[0], maxElementCount);

    uint laneIndex = WaveGetLaneIndex();         // 0..31 or 0..63
    uint laneCount = WaveGetLaneCount();         // 32 or 64
    uint waveIndex = groupIndex / laneCount;     // 0..15 or 0..7
    uint waveCount = WORKGROUP_SIZE / laneCount; // 32 or 16
    uint index = waveIndex * laneCount + laneIndex;

    uint4 waveMask = GetExclusiveWaveMask(laneIndex);

    uint partitionIndex = groupId.x;
    uint partitionStart = partitionIndex * PARTITION_SIZE;

    // This condition is uniform across the workgroup, keeping every barrier converged.
    if (partitionStart < elementCount) {
      for (uint i = index; i < RADIX * waveCount; i += WORKGROUP_SIZE) {
        localHistogram[i] = 0;
      }
      GroupMemoryBarrierWithGroupSync();

      // load from global memory, local histogram and offset
      uint localKeys[PARTITION_DIVISION];
      uint localRadix[PARTITION_DIVISION];
      uint localOffsets[PARTITION_DIVISION];
      uint waveHistogram[PARTITION_DIVISION];
#ifdef KEY_VALUE
      uint localValues[PARTITION_DIVISION];
#endif // KEY_VALUE

      for (int i = 0; i < PARTITION_DIVISION; ++i) {
        uint keyIndex =
          partitionStart + (PARTITION_DIVISION * laneCount) * waveIndex + i * laneCount + laneIndex;
        uint key = keyIndex < elementCount ? keysIn[keyIndex] : 0xffffffff;
        localKeys[i] = key;

#ifdef KEY_VALUE
        localValues[i] = keyIndex < elementCount ? valuesIn[keyIndex] : 0;
#endif // KEY_VALUE

        uint radix = bitfieldExtract(key, pass * 8, 8);
        localRadix[i] = radix;

        // mask per digit
        uint4 mask = WaveActiveBallot(true);
        [ForceUnroll] for (int j = 0; j < 8; ++j) {
          uint digit = (radix >> j) & 1;
          uint4 ballot = WaveActiveBallot(digit == 1);
          // digit - 1 is 0 or 0xffffffff. xor to flip.
          mask &= uint4(digit - 1) ^ ballot;
        }

        // wave level offset for radix
        uint waveOffset = GetBitCount(waveMask & mask);
        uint radixCount = GetBitCount(mask);

        // elect a representative per radix, add to histogram
        if (waveOffset == 0) {
          // accumulate to local histogram
          __atomic_add(localHistogram[waveIndex * RADIX + radix], radixCount,
                       MemoryOrder.Relaxed);
          waveHistogram[i] = radixCount;
        } else {
          waveHistogram[i] = 0;
        }
        localOffsets[i] = waveOffset;
      }
      GroupMemoryBarrierWithGroupSync();

      uint radixTotal = 0;
      for (uint wave = 0; wave < waveCount; ++wave) {
        uint histogramIndex = wave * RADIX + index;
        uint count = localHistogram[histogramIndex];
        localHistogram[histogramIndex] = radixTotal;
        radixTotal += count;
      }

      uint subgroupCount = WaveActiveSum(radixTotal);
      localHistogramSum[index] = WavePrefixSum(radixTotal);
      if (laneIndex == 0) {
        subgroupSums[waveIndex] = subgroupCount;
      }
      GroupMemoryBarrierWithGroupSync();

      uint subgroupPrefix = 0;
      for (uint subgroup = 0; subgroup < waveIndex; ++subgroup) {
        subgroupPrefix += subgroupSums[subgroup];
      }
      localHistogramSum[index] += subgroupPrefix;
      GroupMemoryBarrierWithGroupSync();

      // post-scan stage
      for (int i = 0; i < PARTITION_DIVISION; ++i) {
        uint radix = localRadix[i];
        localOffsets[i] +=
            localHistogramSum[radix] + localHistogram[waveIndex * RADIX + radix];

        WaveBarrier();
        if (waveHistogram[i] > 0) {
          __atomic_add(localHistogram[waveIndex * RADIX + radix], waveHistogram[i],
                       MemoryOrder.Relaxed);
        }
        WaveBarrier();
      }
      GroupMemoryBarrierWithGroupSync();

      // Convert the local radix prefix into the final global base adjustment.
      for (uint i = index; i < RADIX; i += WORKGROUP_SIZE) {
        localHistogramSum[i] = globalHistogram[RADIX * pass + i] +
                               partitionHistogram[RADIX * partitionIndex + i] -
                               localHistogramSum[i];
      }
      GroupMemoryBarrierWithGroupSync();

      // rearrange keys. grouping keys together makes dstOffset to be almost sequential, grants huge
      // speed boost. now localHistogram is unused, so alias memory.
      for (int i = 0; i < PARTITION_DIVISION; ++i) {
        localHistogram[localOffsets[i]] = localKeys[i];
      }
      GroupMemoryBarrierWithGroupSync();

      // binning
      for (uint i = index; i < PARTITION_SIZE; i += WORKGROUP_SIZE) {
        uint key = localHistogram[i];
        uint radix = bitfieldExtract(key, pass * 8, 8);
        uint dstOffset = localHistogramSum[radix] + i;
        if (dstOffset < elementCount) {
          keysOut[dstOffset] = key;
        }

#ifdef KEY_VALUE
        localKeys[i / WORKGROUP_SIZE] = dstOffset;
#endif // KEY_VALUE
      }

#ifdef KEY_VALUE
      GroupMemoryBarrierWithGroupSync();

      for (int i = 0; i < PARTITION_DIVISION; ++i) {
        localHistogram[localOffsets[i]] = localValues[i];
      }
      GroupMemoryBarrierWithGroupSync();

      for (uint i = index; i < PARTITION_SIZE; i += WORKGROUP_SIZE) {
        uint value = localHistogram[i];
        uint dstOffset = localKeys[i / WORKGROUP_SIZE];
        if (dstOffset < elementCount) {
          valuesOut[dstOffset] = value;
        }
      }
#endif // KEY_VALUE
    }
  }
