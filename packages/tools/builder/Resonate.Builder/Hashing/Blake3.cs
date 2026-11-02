using System;
using System.Collections.Generic;

namespace Resonate.Builder.Hashing;

/* BLAKE3 (RFC draft) in one file: the compression function, the chunk tree and
   the root output. The SDK needs the same digest the engine's asset pipeline
   uses; keeping the implementation here means the builder does not reach into
   the native library for a pure algorithm.
 *
 * The chunk tree follows the reference implementation: chunks fold into
 * parent nodes as their count becomes even, the leftover stack folds at the
 * root. Both keyed and unkeyed modes are supported because the CAS uses keyed
   digests and the asset fingerprint uses the plain one. */
internal static class Blake3
{
    internal const int DigestBytes = 32;

    private static readonly uint[] Iv =
    [
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
    ];

    private static readonly byte[] MessagePermutation =
    [
        2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
    ];

    private const uint ChunkStart = 1U << 0;
    private const uint ChunkEnd = 1U << 1;
    private const uint Parent = 1U << 2;
    private const uint Root = 1U << 3;
    private const uint KeyedHash = 1U << 4;
    private const int BlockBytes = 64;
    private const int ChunkBytes = 1024;

    internal static byte[] Hash(ReadOnlySpan<byte> input)
    {
        return HashCore(input, null);
    }

    internal static byte[] HashKeyed(ReadOnlySpan<byte> input, ReadOnlySpan<byte> key)
    {
        if (key.Length != 32)
        {
            throw new ArgumentException("a BLAKE3 key is 32 bytes", nameof(key));
        }
        return HashCore(input, key.ToArray());
    }

    private static byte[] HashCore(ReadOnlySpan<byte> input, byte[]? key)
    {
        uint[] keyWords = new uint[8];
        uint flags = 0;
        if (key is not null)
        {
            for (int index = 0; index < 8; ++index)
            {
                keyWords[index] = ReadUInt32(key.AsSpan(index * 4));
            }
            flags |= KeyedHash;
        }
        else
        {
            Array.Copy(Iv, keyWords, 8);
        }

        int chunkCount = Math.Max(1, (input.Length + ChunkBytes - 1) / ChunkBytes);
        List<uint[]> stack = [];
        for (int chunk = 0; chunk < chunkCount - 1; ++chunk)
        {
            uint[] cv = ChunkChainingValue(input.Slice(chunk * ChunkBytes, ChunkBytes),
                                           (ulong)chunk, keyWords, flags);
            AddChunkCv(stack, cv, (ulong)(chunk + 1), keyWords, flags);
        }

        int lastStart = (chunkCount - 1) * ChunkBytes;
        Output output = ChunkOutput(input[lastStart..], (ulong)(chunkCount - 1), keyWords, flags);
        for (int index = stack.Count - 1; index >= 0; --index)
        {
            output = ParentOutput(stack[index], output.ChainingValue(), keyWords, flags);
        }
        return output.RootBytes();
    }

    private static void AddChunkCv(List<uint[]> stack, uint[] cv, ulong totalChunks, uint[] keyWords,
                                   uint flags)
    {
        while ((totalChunks & 1U) == 0U)
        {
            uint[] left = stack[^1];
            stack.RemoveAt(stack.Count - 1);
            cv = ParentChainingValue(left, cv, keyWords, flags);
            totalChunks >>= 1;
        }
        stack.Add(cv);
    }

    private static uint[] ChunkChainingValue(ReadOnlySpan<byte> chunk, ulong counter,
                                             uint[] keyWords, uint flags)
    {
        return ChunkOutput(chunk, counter, keyWords, flags).ChainingValue();
    }

    /* The chunk's final block with the chunk flags; the caller either takes its
       chaining value (an interior chunk) or folds it into the root. */
    private static Output ChunkOutput(ReadOnlySpan<byte> chunk, ulong counter, uint[] keyWords,
                                      uint flags)
    {
        uint[] cv = new uint[8];
        Array.Copy(keyWords, cv, 8);
        int blocksCompressed = 0;
        int at = 0;
        while (chunk.Length - at > BlockBytes)
        {
            uint[] block = ReadBlock(chunk.Slice(at, BlockBytes));
            CompressInPlace(cv, block, counter, BlockBytes,
                            flags | (blocksCompressed == 0 ? ChunkStart : 0U));
            ++blocksCompressed;
            at += BlockBytes;
        }
        int lastLength = chunk.Length - at;
        uint[] last = ReadBlock(chunk[at..]);
        uint lastFlags = flags | ChunkEnd | (blocksCompressed == 0 ? ChunkStart : 0U);
        return new Output(cv, last, counter, (uint)lastLength, lastFlags);
    }

    private static Output ParentOutput(uint[] left, uint[] right, uint[] keyWords, uint flags)
    {
        uint[] block = new uint[16];
        Array.Copy(left, 0, block, 0, 8);
        Array.Copy(right, 0, block, 8, 8);
        return new Output(keyWords, block, 0UL, BlockBytes, flags | Parent);
    }

    private static uint[] ParentChainingValue(uint[] left, uint[] right, uint[] keyWords,
                                              uint flags)
    {
        return ParentOutput(left, right, keyWords, flags).ChainingValue();
    }

    private static uint[] ReadBlock(ReadOnlySpan<byte> bytes)
    {
        uint[] block = new uint[16];
        for (int index = 0; index < bytes.Length / 4; ++index)
        {
            block[index] = ReadUInt32(bytes.Slice(index * 4));
        }
        int remainder = bytes.Length % 4;
        if (remainder != 0)
        {
            uint tail = 0;
            int baseAt = (bytes.Length / 4) * 4;
            for (int index = 0; index < remainder; ++index)
            {
                tail |= (uint)bytes[baseAt + index] << (8 * index);
            }
            block[bytes.Length / 4] = tail;
        }
        return block;
    }

    private static uint ReadUInt32(ReadOnlySpan<byte> bytes)
    {
        return (uint)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24));
    }

    private static void CompressInPlace(uint[] cv, uint[] block, ulong counter, uint blockLength,
                                        uint flags)
    {
        uint[] state =
        [
            cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
            Iv[0], Iv[1], Iv[2], Iv[3],
            (uint)counter, (uint)(counter >> 32), blockLength, flags,
        ];
        uint[] message = new uint[16];
        Array.Copy(block, message, 16);
        for (int round = 0; round < 7; ++round)
        {
            if (round != 0)
            {
                uint[] permuted = new uint[16];
                for (int index = 0; index < 16; ++index)
                {
                    permuted[index] = message[MessagePermutation[index]];
                }
                message = permuted;
            }
            Mix(state, message, 0, 4, 8, 12, 0, 1);
            Mix(state, message, 1, 5, 9, 13, 2, 3);
            Mix(state, message, 2, 6, 10, 14, 4, 5);
            Mix(state, message, 3, 7, 11, 15, 6, 7);
            Mix(state, message, 0, 5, 10, 15, 8, 9);
            Mix(state, message, 1, 6, 11, 12, 10, 11);
            Mix(state, message, 2, 7, 8, 13, 12, 13);
            Mix(state, message, 3, 4, 9, 14, 14, 15);
        }
        for (int index = 0; index < 8; ++index)
        {
            cv[index] = state[index] ^ state[index + 8];
        }
    }

    private static void Mix(uint[] state, uint[] message, int a, int b, int c, int d, int mx, int my)
    {
        state[a] = state[a] + state[b] + message[mx];
        state[d] = RotateRight(state[d] ^ state[a], 16);
        state[c] += state[d];
        state[b] = RotateRight(state[b] ^ state[c], 12);
        state[a] = state[a] + state[b] + message[my];
        state[d] = RotateRight(state[d] ^ state[a], 8);
        state[c] += state[d];
        state[b] = RotateRight(state[b] ^ state[c], 7);
    }

    private static uint RotateRight(uint value, int bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    /* A pending compression: its input chaining value, final block, counter and
       flags. The root output is this compression with the ROOT flag, read as
       64 bytes; a chaining value is its first eight words. */
    private readonly struct Output
    {
        private readonly uint[] _inputCv;
        private readonly uint[] _block;
        private readonly ulong _counter;
        private readonly uint _blockLength;
        private readonly uint _flags;

        internal Output(uint[] inputCv, uint[] block, ulong counter, uint blockLength,
                        uint flags)
        {
            _inputCv = inputCv;
            _block = block;
            _counter = counter;
            _blockLength = blockLength;
            _flags = flags;
        }

        internal uint[] ChainingValue()
        {
            uint[] cv = new uint[8];
            Array.Copy(_inputCv, cv, 8);
            CompressInPlace(cv, _block, _counter, _blockLength, _flags);
            return cv;
        }

        internal byte[] RootBytes()
        {
            uint[] cv = new uint[8];
            Array.Copy(_inputCv, cv, 8);
            uint[] block = new uint[16];
            Array.Copy(_block, block, 16);
            uint[] state = BuildState(cv, block, _counter, _blockLength, _flags | Root);
            byte[] bytes = new byte[DigestBytes];
            for (int index = 0; index < 8; ++index)
            {
                uint word = state[index] ^ state[index + 8];
                bytes[index * 4] = (byte)word;
                bytes[index * 4 + 1] = (byte)(word >> 8);
                bytes[index * 4 + 2] = (byte)(word >> 16);
                bytes[index * 4 + 3] = (byte)(word >> 24);
            }
            return bytes;
        }

        private static uint[] BuildState(uint[] cv, uint[] block, ulong counter, uint blockLength,
                                         uint flags)
        {
            uint[] state =
            [
                cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
                Iv[0], Iv[1], Iv[2], Iv[3],
                (uint)counter, (uint)(counter >> 32), blockLength, flags,
            ];
            uint[] message = new uint[16];
            Array.Copy(block, message, 16);
            for (int round = 0; round < 7; ++round)
            {
                if (round != 0)
                {
                    uint[] permuted = new uint[16];
                    for (int index = 0; index < 16; ++index)
                    {
                        permuted[index] = message[MessagePermutation[index]];
                    }
                    message = permuted;
                }
                Mix(state, message, 0, 4, 8, 12, 0, 1);
                Mix(state, message, 1, 5, 9, 13, 2, 3);
                Mix(state, message, 2, 6, 10, 14, 4, 5);
                Mix(state, message, 3, 7, 11, 15, 6, 7);
                Mix(state, message, 0, 5, 10, 15, 8, 9);
                Mix(state, message, 1, 6, 11, 12, 10, 11);
                Mix(state, message, 2, 7, 8, 13, 12, 13);
                Mix(state, message, 3, 4, 9, 14, 14, 15);
            }
            return state;
        }
    }
}
