package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.io.Writer

class SessionLogWriterTest {
    private class RecordingWriter(private val fail: Boolean = false) : Writer() {
        val text = StringBuilder()
        var closes = 0
        var flushes = 0
        override fun write(chars: CharArray, offset: Int, length: Int) {
            check(closes == 0) { "write after close" }
            if (fail) throw IOException("test write failed")
            text.append(chars, offset, length)
        }
        override fun flush() { flushes++ }
        override fun close() { closes++ }
    }

    @Test fun oldActivityCleanupCannotCloseTheNextSession() {
        val log = SessionLogWriter()
        val first = RecordingWriter()
        val oldOwner = log.start { first }
        assertTrue(log.write("first\n"))
        val second = RecordingWriter()
        val currentOwner = log.start { second }
        assertEquals(1, first.closes)
        assertFalse(log.close(oldOwner))
        assertTrue(log.write("second\n"))
        assertEquals("first\n", first.text.toString())
        assertEquals("second\n", second.text.toString())
        assertEquals(0, second.closes)
        assertEquals(1, second.flushes)
        assertTrue(log.close(currentOwner))
        assertFalse(log.close(currentOwner))
        assertEquals(1, second.closes)
    }

    @Test fun rapidRestartsHaveUniqueOwnersAndRetiredOwnersStayHarmless() {
        val log = SessionLogWriter()
        val writers = mutableListOf<RecordingWriter>()
        val owners = (1..1000).map {
            log.start { RecordingWriter().also(writers::add) }
        }
        assertEquals(1000, owners.toSet().size)
        owners.dropLast(1).forEach { assertFalse(log.close(it)) }
        assertFalse(log.close(0))
        assertTrue(log.write("current\n"))
        writers.dropLast(1).forEach { assertEquals(1, it.closes) }
        assertEquals("current\n", writers.last().text.toString())
        assertEquals(0, writers.last().closes)
        assertTrue(log.close(owners.last()))
        assertEquals(1, writers.last().closes)
    }

    @Test fun failedWritesCloseOnceAndDoNotPreventANewSession() {
        val log = SessionLogWriter()
        val broken = RecordingWriter(fail = true)
        val brokenOwner = log.start { broken }
        assertFalse(log.write("lost\n"))
        assertFalse(log.write("still closed\n"))
        assertFalse(log.close(brokenOwner))
        assertEquals(1, broken.closes)
        val next = RecordingWriter()
        val owner = log.start { next }
        assertFalse(log.close(brokenOwner))
        assertTrue(log.write("saved\n"))
        assertEquals("saved\n", next.text.toString())
        assertTrue(log.close(owner))
    }

    @Test fun failedOpenStillInvalidatesThePreviousOwner() {
        val log = SessionLogWriter()
        val old = RecordingWriter()
        val oldOwner = log.start { old }
        val failedOwner = log.start { null }
        assertEquals(1, old.closes)
        assertFalse(log.close(oldOwner))
        assertFalse(log.close(failedOwner))
        assertFalse(log.write("no writer\n"))
        val next = RecordingWriter()
        val owner = log.start { next }
        assertTrue(owner != oldOwner && owner != failedOwner)
        assertFalse(log.close(oldOwner))
        assertFalse(log.close(failedOwner))
        assertTrue(log.write("survived\n"))
        assertEquals("survived\n", next.text.toString())
    }
}
