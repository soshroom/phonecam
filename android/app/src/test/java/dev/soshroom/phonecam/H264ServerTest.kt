package dev.soshroom.phonecam

import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class H264ServerTest {
    @Test fun notifiesWhenLastClientDisconnects() {
        val port = ServerSocket(0).use { it.localPort }
        val connected = CountDownLatch(1)
        val disconnected = CountDownLatch(1)
        val server = H264Server(port) { isConnected ->
            if (isConnected) connected.countDown() else disconnected.countDown()
        }
        try {
            server.start()
            assertEquals(0, server.clientCount())
            Socket("127.0.0.1", port).use {
                assertTrue(connected.await(5, TimeUnit.SECONDS))
                assertEquals(1, server.clientCount())
            }
            assertTrue(disconnected.await(5, TimeUnit.SECONDS))
            assertEquals(0, server.clientCount())
        } finally {
            server.stop()
        }
    }
}
