package phone.webcam.phonewebcam.ui.Theme

import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.foundation.isSystemInDarkTheme

//Dark Mode
private val DarkColors = darkColorScheme(
    primary = Purple80,      // Lighter purple for dark mode readability
    secondary = Teal700,
    tertiary = PurpleDark,
    background = DeepGrey,      // Using your Black from Color.kt
    surface = NearlyBlack, // Standard dark surface

)

// Light Mode
private val LightColors = lightColorScheme(
    primary = PurpleDark,
    secondary = Teal700,
    background = ShallowGrey,
    surface = NearlyWhite,
)

@Composable
fun PhoneWebcamTheme(
    darkTheme: Boolean = isSystemInDarkTheme(), // Check system setting
    content: @Composable () -> Unit
) {
    // 3. Select which color scheme to use
    val colorScheme = if (darkTheme) DarkColors else LightColors

    MaterialTheme(
        colorScheme = colorScheme,
        //typography = Typography, // Added your typography from Type.kt
        content = content
    )
}