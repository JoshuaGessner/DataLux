package dev.fileflow

import dev.fileflow.rx.Grids
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SpeedCompatibilityTest {
    @Test fun webcamModeUsesADecodableLowerDensityBlackAndWhiteGrid() {
        val webcam = Speed.Webcam
        assertTrue(Grids.ALL.contains(webcam.cols to webcam.rows))
        assertTrue(webcam.cols < Speed.Steady.cols && webcam.rows < Speed.Steady.rows)
        assertEquals(0, webcam.colours)
        assertTrue(webcam.statesPerSecond <= 20.0)
        assertEquals(Speed.Fast, Speed.DEFAULT)
    }
}
