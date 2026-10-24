package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ThroughputTest {

    @Test fun compressionThatPaysIsCountedAsASaving() {
        // 1 MB as 250 KB at 100 KB/s: 2.5 s on the link, plus 0.5 s compressing and 0.1 s opening,
        // against 10 s sent as it is.
        val t = Throughput(1_000_000, 250_000, 100_000.0, compressMs = 500, decompressMs = 100)
        assertEquals(4.0, t.ratio, 1e-9)
        assertEquals(3.1, t.totalSeconds, 1e-9)
        assertEquals(10.0, t.uncompressedSeconds, 1e-9)
        assertEquals(6.9, t.secondsSaved, 1e-9)
        assertEquals(1_000_000 / 3.1, t.effectiveRate, 1e-6)
    }

    @Test fun compressionThatCostsMoreThanItSavesIsALoss() {
        // 2% smaller, but 2 s to get there.
        val t = Throughput(1_000_000, 980_000, 100_000.0, compressMs = 2000)
        assertTrue(t.secondsSaved < 0)
        assertTrue(t.effectiveRate < t.linkRate)
        assertEquals("costs 1.8 s", Throughput.describeSaving(t.secondsSaved))
    }

    @Test fun nothingKnownIsNotDividedBy() {
        val t = Throughput(1_000_000, 250_000, 0.0)
        assertFalse(t.valid)
        assertEquals(0.0, t.effectiveRate, 0.0)
        assertEquals(0.0, t.secondsSaved, 0.0)
    }

    @Test fun savingsAreSaidInWords() {
        assertEquals("saves 12 s", Throughput.describeSaving(12.3))
        assertEquals("saves 0.4 s", Throughput.describeSaving(0.4))
        assertEquals("costs 3.0 s", Throughput.describeSaving(-3.0))
    }
}
