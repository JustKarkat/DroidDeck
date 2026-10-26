package com.droiddeck.launcher.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.sp
import com.droiddeck.launcher.R

/** Logo mark + app name, used wherever the old hand-drawn wordmark appeared. */
@Composable
internal fun Wordmark(height: Dp, modifier: Modifier = Modifier) {
    val ink = MaterialTheme.colorScheme.onBackground
    val name = stringResource(R.string.app_name)
    val density = LocalDensity.current
    Row(modifier.semantics { contentDescription = name }, verticalAlignment = Alignment.CenterVertically) {
        Image(
            painter = painterResource(R.drawable.logo),
            contentDescription = null,
            modifier = Modifier.height(height),
        )
        Spacer(Modifier.width(height * 0.22f))
        Text(
            text = name,
            color = ink,
            fontSize = with(density) { (height * 0.68f).toSp() },
            fontWeight = FontWeight.Bold,
            letterSpacing = (-0.5).sp,
        )
    }
}
