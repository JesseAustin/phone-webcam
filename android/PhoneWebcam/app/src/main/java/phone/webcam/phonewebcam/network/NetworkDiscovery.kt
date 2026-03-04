package phone.webcam.phonewebcam.network

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

class NetworkDiscovery(private val context: Context) {

    companion object {
        private const val TAG = "PhoneWebcam"
        const val SERVICE_TYPE = "_phonewebcam._udp."
        const val SERVICE_NAME = "PhoneWebcam"
    }

    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var registrationListener: NsdManager.RegistrationListener? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var pendingRestartPort: Int? = null
    private var currentPort = 9000

    fun startAdvertising(port: Int) {
        if (registrationListener != null) {
            Log.w(TAG, "Already advertising")
            return
        }
        currentPort = port
        registerService(port)
    }

    private fun registerService(port: Int) {
        val serviceInfo = NsdServiceInfo().apply {
            serviceName = SERVICE_NAME
            serviceType = SERVICE_TYPE
            setPort(port)
        }

        registrationListener = object : NsdManager.RegistrationListener {
            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "mDNS registration failed: $errorCode")
            }
            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "mDNS unregistration failed: $errorCode")
            }
            override fun onServiceRegistered(info: NsdServiceInfo) {
                Log.i(TAG, "mDNS advertising started: ${info.serviceName} on port $port")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {
                Log.i(TAG, "mDNS advertising stopped")
                registrationListener = null
                pendingRestartPort?.let { port ->
                    pendingRestartPort = null
                    registerService(port)
                }
            }
        }

        nsdManager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD,
            registrationListener!!)
    }

    fun stopAdvertising() {
        registrationListener?.let {
            try {
                nsdManager.unregisterService(it)
            } catch (e: Exception) {
                Log.w(TAG, "mDNS unregister error: ${e.message}")
            }
            registrationListener = null
        }
    }

    fun restartAdvertising(port: Int) {
        val listener = registrationListener
        if (listener == null) {
            startAdvertising(port)
            return
        }
        // Defer the restart until unregistration completes
        pendingRestartPort = port
        nsdManager.unregisterService(listener)
        // onServiceUnregistered will call startAdvertising(pendingRestartPort)
    }

    fun stop() {
        scope.cancel()
        stopAdvertising()
    }
}