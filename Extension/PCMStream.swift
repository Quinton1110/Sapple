//
//  PCMStream.swift
//  Hand-off between the synthesis thread (producer) and the audio unit's render block (consumer),
//  so audio starts playing while the rest of the utterance is still being synthesized.
//
//  Every utterance gets a generation number. Cancelling bumps it, which makes the producer's next
//  append fail (so the engine stops within one chunk) and lets the render block report completion.
//

import Foundation

final class PCMStream {
    private let cond = NSCondition()
    private var buf: [Float32] = []
    private var readPos = 0
    private var gen: UInt64 = 0
    private var finished = true          // nothing queued: a render completes at once
    private var lastRead = Date()

    /// Keep at most this much unread audio buffered; the engine is far faster than real time.
    private let maxBuffered = Int(ClassicEngine.sampleRate) * 20
    /// Give up on a producer whose consumer has stopped pulling (the audio unit went away).
    private let abandonAfter: TimeInterval = 15

    // MARK: - Producer

    /// Starts a new utterance, discarding anything left of the previous one.
    func begin() -> UInt64 {
        cond.lock(); defer { cond.unlock() }
        gen &+= 1
        buf.removeAll(keepingCapacity: true)
        readPos = 0
        finished = false
        lastRead = Date()
        cond.broadcast()
        return gen
    }

    /// Appends engine PCM. Returns false once this utterance is cancelled or superseded.
    func append(_ g: UInt64, _ pcm: UnsafeBufferPointer<Int16>) -> Bool {
        cond.lock(); defer { cond.unlock() }
        guard waitForRoom(g) else { return false }
        compact()
        buf.reserveCapacity(buf.count + pcm.count)
        let scale: Float32 = 1.0 / 32768.0
        for s in pcm { buf.append(Float32(s) * scale) }
        cond.broadcast()
        return true
    }

    /// Appends `ms` of silence (for SSML breaks and the short end pad).
    @discardableResult
    func appendSilence(_ g: UInt64, ms: Int) -> Bool {
        let n = Int(Double(ms) / 1000.0 * ClassicEngine.sampleRate)
        cond.lock(); defer { cond.unlock() }
        guard n > 0, waitForRoom(g) else { return g == gen && !finished }
        compact()
        buf.append(contentsOf: repeatElement(0, count: n))
        cond.broadcast()
        return true
    }

    func finish(_ g: UInt64) {
        cond.lock(); defer { cond.unlock() }
        if g == gen { finished = true }
        cond.broadcast()
    }

    /// Stops whatever is playing or being synthesized.
    func cancel() {
        cond.lock(); defer { cond.unlock() }
        gen &+= 1
        buf.removeAll(keepingCapacity: true)
        readPos = 0
        finished = true
        cond.broadcast()
    }

    /// Blocks (bounded) until the utterance has audio or is over.
    func waitForAudio(_ g: UInt64, timeout: TimeInterval) {
        let deadline = Date(timeIntervalSinceNow: timeout)
        cond.lock(); defer { cond.unlock() }
        while g == gen && !finished && buf.count == readPos {
            if !cond.wait(until: deadline) { break }
        }
    }

    // caller holds the lock
    private func waitForRoom(_ g: UInt64) -> Bool {
        while g == gen && !finished && buf.count - readPos > maxBuffered {
            if Date().timeIntervalSince(lastRead) > abandonAfter { return false }
            _ = cond.wait(until: Date(timeIntervalSinceNow: 0.25))
        }
        return g == gen && !finished
    }

    // caller holds the lock; the producer pays for the memmove, never the render thread
    private func compact() {
        if readPos > 32768 && readPos * 2 > buf.count {
            buf.removeFirst(readPos)
            readPos = 0
        }
    }

    // MARK: - Consumer (render block)

    /// Copies up to `frames` samples into `out`. Waits (at most `maxWait`) if the producer has not
    /// caught up yet. `done` = the utterance is over and everything has been handed out.
    func read(into out: UnsafeMutablePointer<Float32>, frames: Int, maxWait: TimeInterval) -> (count: Int, done: Bool) {
        cond.lock(); defer { cond.unlock() }
        if !finished && buf.count == readPos {
            let deadline = Date(timeIntervalSinceNow: maxWait)
            while !finished && buf.count == readPos {
                if !cond.wait(until: deadline) { break }
            }
        }
        let n = min(buf.count - readPos, frames)
        if n > 0 {
            buf.withUnsafeBufferPointer { p in
                out.update(from: p.baseAddress!.advanced(by: readPos), count: n)
            }
            readPos += n
            lastRead = Date()
            cond.broadcast()   // room for a producer waiting on the buffer limit
        }
        let done = finished && readPos >= buf.count
        if done {
            buf.removeAll(keepingCapacity: true)
            readPos = 0
        }
        return (n, done)
    }
}
