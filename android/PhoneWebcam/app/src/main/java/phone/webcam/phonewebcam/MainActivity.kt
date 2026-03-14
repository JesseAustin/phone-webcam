package phone.webcam.phonewebcam

import android.Manifest
import android.R
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.accompanist.permissions.ExperimentalPermissionsApi
import com.google.accompanist.permissions.isGranted
import com.google.accompanist.permissions.rememberPermissionState
import phone.webcam.phonewebcam.ui.StreamViewModel
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import phone.webcam.phonewebcam.camera.CameraManager
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.ui.draw.clip
import android.content.res.Configuration
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.material.icons.filled.CameraFront
import androidx.compose.material.icons.filled.CameraRear
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.accompanist.permissions.rememberMultiplePermissionsState
import phone.webcam.phonewebcam.ui.Theme.PhoneWebcamTheme

class MainActivity : ComponentActivity() {

    private val viewModel: StreamViewModel by viewModels()

    private var savedResolutionW: Int = 1920
    private var savedResolutionH: Int = 1080

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {

            PhoneWebcamTheme {
                Surface(
                    modifier = Modifier.windowInsetsPadding(WindowInsets.systemBars),
                    color = MaterialTheme.colorScheme.background
                ) {
                    StreamScreen(viewModel)
                }
            }
        }
    }

    // Use this as a last resort if background streaming is too slow:
    override fun onStop() {
        super.onStop()
        if (viewModel.uiState.value.isStreaming) {
            viewModel.stopStreaming()
        }
    }


}

@Composable
fun CameraPreview(
    //selectedResolution: CameraManager.StreamResolution,
    onSurfaceReady: (android.view.Surface?) -> Unit
) {

    val configuration = LocalConfiguration.current
    val isLandscape = configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    val modifier = if (isLandscape) {
        Modifier
            .fillMaxWidth()
            .aspectRatio(16f / 9f)
    } else {
        // Portrait: fill width, fixed height that feels natural, clip overflow
        Modifier
            .fillMaxWidth()
            .height(320.dp)  // tweak to taste
    }

    AndroidView(
        factory = { context ->
            SurfaceView(context).also { surfaceView ->
                surfaceView.holder.addCallback(object : android.view.SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: android.view.SurfaceHolder) {
                        onSurfaceReady(holder.surface)
                    }
                    override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) {}
                    override fun surfaceDestroyed(holder: android.view.SurfaceHolder) {
                        onSurfaceReady(null)
                    }
                })
            }
        },
        modifier = modifier.clip(RoundedCornerShape(12.dp))
    )
}

@OptIn(ExperimentalPermissionsApi::class)
@Composable
fun StreamScreen(viewModel: StreamViewModel) {

    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val focusManager = LocalFocusManager.current // Fixes "Unresolved reference focusManager"
    //val cameraPermissionState = rememberPermissionState(Manifest.permission.CAMERA)
    //val audioPermissionState = rememberPermissionState(Manifest.permission.RECORD_AUDIO)


    // 1. Combine permissions into one state
    val permissionsState = rememberMultiplePermissionsState(
        permissions = listOf(
            Manifest.permission.CAMERA,
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.POST_NOTIFICATIONS
        )
    )

    // 2. Request them all at once
    LaunchedEffect(Unit) {
        if (!permissionsState.allPermissionsGranted) {
            permissionsState.launchMultiplePermissionRequest()
        }
    }

    // Local state for the show/hide toggle — doesn't need to survive recomposition
    var passwordVisible by remember { mutableStateOf(false) }

    Scaffold(
        bottomBar = {
            Surface (color = MaterialTheme.colorScheme.surface)
            {
                Column(
                    modifier = Modifier
                        .navigationBarsPadding()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
                    val noPassword = uiState.password.isEmpty() && !uiState.isStreaming
                    Button(
                        onClick = {
                            if (uiState.isStreaming) {
                                viewModel.stopStreaming()
                            } else {
                                viewModel.startStreaming()
                            }
                        },
                        enabled = !noPassword,
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(56.dp),
                        colors = if (uiState.isStreaming) {
                            ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                        } else {
                            ButtonDefaults.buttonColors()
                        }
                    ) {
                        Text(
                            text = if (uiState.isStreaming) "STOP STREAM"
                            else if (noPassword) "SET A PASSWORD TO STREAM"
                            else "START STREAM",
                            style = MaterialTheme.typography.titleMedium
                        )
                    }
                }
            }
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(innerPadding)
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Row() {
                    Image(
                        painter = painterResource(id = phone.webcam.phonewebcam.R.drawable.ic_launcher_foreground),
                        contentDescription = "Phone Webcam logo",
                        modifier = Modifier
                            .size(62.dp)
                        //.align(Alignment.CenterHorizontally)
                    )

                    // Title
                    Text(
                        text = "Phone Webcam",
                        style = MaterialTheme.typography.titleLarge,
                        modifier = Modifier.align(Alignment.CenterVertically),
                    )
                }

                Box(modifier = Modifier.fillMaxWidth())
                {
                    // Camera Preview
                    CameraPreview(
                        //selectedResolution = uiState.selectedResolution,
                        onSurfaceReady = { surface -> viewModel.setPreviewSurface(surface)
                        })

                    // Grey overlay when not streaming — prevents frozen last-frame
                    if (!uiState.isStreaming) {
                        Box(
                            modifier = Modifier
                                .matchParentSize()
                                .clip(RoundedCornerShape(12.dp))
                                .background(Color(0xCC1A1A1A))
                        )
                    }

                    IconButton(
                        onClick = { viewModel.toggleCamera() },
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(8.dp)
                    ) {
                        Icon(
                            imageVector = if (uiState.isFrontCamera)
                                Icons.Default.CameraFront
                            else
                                Icons.Default.CameraRear,
                            contentDescription = if (uiState.isFrontCamera)
                                "Switch to rear camera"
                            else
                                "Switch to front camera",
                            tint = Color.White
                        )
                    }

                    // New mic toggle — bottom end
                    IconButton(
                        onClick = { viewModel.toggleMic() },
                        modifier = Modifier
                            .align(Alignment.BottomEnd)
                            .padding(8.dp)
                    ) {
                        Icon(
                            imageVector = if (uiState.isMicEnabled) Icons.Default.Mic else Icons.Default.MicOff,
                            contentDescription = if (uiState.isMicEnabled) "Mute mic" else "Unmute mic",
                            tint = if (uiState.isMicEnabled) Color.White else Color.Red
                        )
                    }

                    // LIVE badge — top start, only while streaming
                    if (uiState.isStreaming) {
                        Row(
                            modifier = Modifier
                                .align(Alignment.BottomStart)
                                .padding(8.dp)
                                .background(
                                    color = Color(0xFFCC0000),
                                    shape = RoundedCornerShape(6.dp)
                                )
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(4.dp)
                        ) {
                            Box(
                                modifier = Modifier
                                    .size(7.dp)
                                    .background(Color.White, shape = RoundedCornerShape(50))
                            )
                            Text(
                                text = "LIVE",
                                color = Color.White,
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = androidx.compose.ui.text.font.FontWeight.Bold
                            )
                        }
                    }

                    // OBS link status indicator — bottom start, hidden while streaming (LIVE badge takes over)
                    // Manual mode: all three fields must match what OBS last successfully handshaked with.
                    // Auto mode: driven by status message since fields are filled by OBS automatically.
                    val isObsLinked = if (uiState.autoDiscoveryEnabled) {
                        uiState.targetIp.matches(Regex("""\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}""")) &&
                                !uiState.statusMessage.startsWith("Waiting") &&
                                !uiState.statusMessage.startsWith("Port Mismatch") &&
                                !uiState.statusMessage.startsWith("Authenticating") &&
                                !uiState.statusMessage.startsWith("Starting") &&
                                uiState.statusMessage != "Ready to stream"
                    } else {
                        uiState.lastLinkedIp.isNotEmpty() &&
                                uiState.targetIp == uiState.lastLinkedIp &&
                                uiState.targetPort == uiState.lastLinkedPort &&
                                uiState.password == uiState.lastLinkedPassword
                    }
                    if (!uiState.isStreaming) {
                        Row(
                            modifier = Modifier
                                .align(Alignment.BottomStart)
                                .padding(8.dp)
                                .background(
                                    color = Color(0xCC2A2A2A),
                                    shape = RoundedCornerShape(8.dp)
                                )
                                .padding(horizontal = 10.dp, vertical = 6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            Icon(
                                imageVector = if (isObsLinked) Icons.Default.Wifi else Icons.Default.WifiOff,
                                contentDescription = null,
                                tint = if (isObsLinked) Color(0xFF66FF66) else Color(0xFFAAAAAA),
                                modifier = Modifier.size(16.dp)
                            )
                            Text(
                                text = if (isObsLinked) "Ready to Stream!" else "Waiting for OBS...",
                                color = if (isObsLinked) Color(0xFF66FF66) else Color(0xFFCCCCCC),
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = androidx.compose.ui.text.font.FontWeight.Medium
                            )
                        }
                    }
                }

                // Configuration Card
                Card(
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = "Stream Configuration",
                            style = MaterialTheme.typography.titleMedium
                        )

                        // IP Address Input
                        OutlinedTextField(
                            value = if (uiState.autoDiscoveryEnabled) uiState.targetIp else uiState.targetIp,
                            onValueChange = { if (!uiState.autoDiscoveryEnabled) viewModel.updateIp(it) },
                            label = { Text("IP Address: \uD83C\uDF10") },
                            placeholder = { Text(if (uiState.autoDiscoveryEnabled) "Auto-discovering..." else "192.168.1.100") },
                            // Field is read-only when auto-discovery is active — OBS fills it in
                            enabled = !uiState.isStreaming && !uiState.autoDiscoveryEnabled,
                            trailingIcon = {
                                IconButton(onClick = { viewModel.toggleDiscoveryMode() }) {
                                    Icon(
                                        imageVector = if (uiState.autoDiscoveryEnabled)
                                            Icons.Default.Wifi      // auto mode: shows Wi-Fi icon
                                        else
                                            Icons.Default.Edit,     // manual mode: shows edit icon
                                        contentDescription = if (uiState.autoDiscoveryEnabled)
                                            "Switch to manual IP entry"
                                        else
                                            "Switch to auto-discovery",
                                        tint = if (uiState.autoDiscoveryEnabled)
                                            MaterialTheme.colorScheme.primary  // highlighted when active
                                        else
                                            MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                            },
                            modifier = Modifier.fillMaxWidth(),
                            singleLine = true
                        )

                        // Port Input
                        val portMismatch = uiState.statusMessage.startsWith("Port Mismatch")
                        OutlinedTextField(
                            value = uiState.portText,
                            onValueChange = { newValue ->
                                viewModel.updatePort(newValue)
                            },
                            label = {
                                Text(
                                    "Port: \uD83D\uDEDC",
                                    color = if (portMismatch) MaterialTheme.colorScheme.error
                                    else Color.Unspecified
                                )
                            },
                            isError = portMismatch,
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Number,
                                imeAction = ImeAction.Done
                            ),
                            keyboardActions = KeyboardActions(
                                onDone = {
                                    viewModel.finalizePort()
                                    focusManager.clearFocus()
                                }
                            ),
                            modifier = Modifier
                                .fillMaxWidth()
                                .onFocusChanged { focusState ->
                                    if (!focusState.isFocused) {
                                        viewModel.finalizePort()
                                    }
                                }
                        )

                        // ── Password field ───────────────────────────────────
                        // Mirrors the port field pattern: every keystroke calls
                        // viewModel.updatePassword(), which pushes to HandshakeServer
                        // immediately. No button needed. Leave blank = no auth.
                        OutlinedTextField(
                            value = uiState.password,
                            onValueChange = { viewModel.updatePassword(it) },
                            label = { Text("Password: \uD83D\uDD10") },
                            placeholder = { Text("Password required to stream!") },
                            visualTransformation = if (passwordVisible)
                                VisualTransformation.None
                            else
                                PasswordVisualTransformation(),
                            leadingIcon = {
                                Icon(
                                    imageVector = Icons.Default.Lock,
                                    contentDescription = null,
                                    tint = if (uiState.password.isNotEmpty())
                                        MaterialTheme.colorScheme.primary
                                    else
                                        MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            },
                            trailingIcon = {
                                IconButton(onClick = { passwordVisible = !passwordVisible }) {
                                    Icon(
                                        imageVector = if (passwordVisible)
                                            Icons.Default.VisibilityOff
                                        else
                                            Icons.Default.Visibility,
                                        contentDescription = if (passwordVisible)
                                            "Hide password"
                                        else
                                            "Show password"
                                    )
                                }
                            },
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                            modifier = Modifier.fillMaxWidth(),
                            singleLine = true
                        )
                        // ────────────────────────────────────────────────────

                        // Quality Slider
                        /*Column {
                            Text(
                                text = "JPEG Quality: ${uiState.jpegQuality}",
                                style = MaterialTheme.typography.bodyMedium
                            )
                            Slider(
                                value = uiState.jpegQuality.toFloat(),
                                onValueChange = { viewModel.updateQuality(it.toInt()) },
                                valueRange = 60f..95f,
                                steps = 6,
                                enabled = !uiState.isStreaming,
                                modifier = Modifier.fillMaxWidth()
                            )
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Text("Lower (faster)", style = MaterialTheme.typography.bodySmall)
                                Text("Higher (better)", style = MaterialTheme.typography.bodySmall)
                            }
                        }*/


                        // Bitrate slider:
                        Column {
                            Text(
                                text = "Bitrate: ${uiState.bitrateMbps} Mbps",
                                style = MaterialTheme.typography.bodyMedium
                            )
                            Slider(
                                value = uiState.bitrateMbps.toFloat(),
                                onValueChange = { viewModel.updateBitrate(it.toInt()) },
                                valueRange = 8f..40f,
                                steps = 7,  // 8, 12, 16, 20, 24, 28, 32, 36, 40
                                enabled = !uiState.isStreaming,
                                modifier = Modifier.fillMaxWidth()
                            )
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Text("8 Mbps", style = MaterialTheme.typography.bodySmall)
                                Text("40 Mbps", style = MaterialTheme.typography.bodySmall)
                            }
                        }


                        // Resolution picker dropdown menu:
                        var resolutionExpanded by remember { mutableStateOf(false) }

                        Column {
                            Text(
                                text = "Resolution",
                                style = MaterialTheme.typography.bodyMedium
                            )
                            Box(modifier = Modifier.fillMaxWidth()) {
                                OutlinedTextField(
                                    value = uiState.selectedResolution.label,
                                    onValueChange = {},
                                    readOnly = true,
                                    enabled = !uiState.isStreaming,
                                    trailingIcon = {
                                        IconButton(onClick = {
                                            if (!uiState.isStreaming) resolutionExpanded = true
                                        }) {
                                            Icon(
                                                Icons.Default.ArrowDropDown,
                                                contentDescription = "Select resolution"
                                            )
                                        }
                                    },
                                    modifier = Modifier.fillMaxWidth()
                                )
                                DropdownMenu(
                                    expanded = resolutionExpanded,
                                    onDismissRequest = { resolutionExpanded = false },
                                    modifier = Modifier.fillMaxWidth()
                                ) {
                                    uiState.supportedResolutions.forEach { resolution ->
                                        DropdownMenuItem(
                                            text = { Text(resolution.label) },
                                            onClick = {
                                                viewModel.updateResolution(resolution)
                                                resolutionExpanded = false
                                            }
                                        )
                                    }
                                }
                            }
                        }

                    }
                }

                // Status Card
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = if (uiState.isStreaming) {
                        CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)
                    } else {
                        CardDefaults.cardColors()
                    }
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Text(
                            text = "Status",
                            style = MaterialTheme.typography.titleMedium
                        )

                        Text(
                            text = uiState.statusMessage,
                            style = MaterialTheme.typography.bodyMedium
                        )

                        if (uiState.isStreaming) {
                            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Column {
                                    Text(
                                        text = "Frame Rate",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                    Text(
                                        text = "${uiState.fps} fps",
                                        style = MaterialTheme.typography.titleLarge
                                    )
                                }

                                Column {
                                    Text(
                                        text = "Bandwidth",
                                        style = MaterialTheme.typography.bodySmall
                                    )
                                    Text(
                                        text = "%.1f Mbps".format(uiState.bandwidth),
                                        style = MaterialTheme.typography.titleLarge
                                    )
                                }
                            }

                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Text(
                                    text = "Frames: ${uiState.frameCount}",
                                    style = MaterialTheme.typography.bodyMedium
                                )
                            }
                        }
                    }
                }

                // Find the camera permission status from the combined list
                val cameraPermissionState = permissionsState.permissions.find { it.permission == Manifest.permission.CAMERA }

                // Permission Request
                if (cameraPermissionState?.status?.isGranted == false && !uiState.isStreaming) {
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer
                        )
                    ) {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp)
                        ) {
                            Text(
                                text = "Camera Permission Required",
                                style = MaterialTheme.typography.titleSmall,
                                color = MaterialTheme.colorScheme.onErrorContainer
                            )
                            Text(
                                text = "Please grant camera permission to start streaming",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onErrorContainer
                            )
                        }
                    }
                }

                // Info Text
                Text(
                    text = if (uiState.isStreaming) {
                        "Streaming ${uiState.selectedResolution.label} to OBS via UDP"
                    } else {
                        "Stream ${uiState.selectedResolution.label} to OBS via UDP"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.align(Alignment.CenterHorizontally)
                )
            }
        }
    }
}