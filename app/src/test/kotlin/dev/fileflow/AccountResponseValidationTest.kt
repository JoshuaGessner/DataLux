package dev.fileflow

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class AccountResponseValidationTest {
    @Test fun acceptsOnlyActualBooleanValues() {
        assertEquals(true, requireAccountBoolean(true))
        assertEquals(false, requireAccountBoolean(false))
        for (invalid in listOf(null, "true", "false", 0, 1, Any())) {
            assertThrows(AccountError::class.java) { requireAccountBoolean(invalid) }
        }
    }
}
