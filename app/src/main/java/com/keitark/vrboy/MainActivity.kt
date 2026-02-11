package com.keitark.vrboy

import android.app.NativeActivity
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.util.Log

class MainActivity : NativeActivity() {
    external fun nativeOnRomSelected(data: ByteArray, displayName: String)
    external fun nativeOnRomPickerDismissed()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
    }

    fun openRomPicker() {
        runOnUiThread {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
            }
            startActivityForResult(intent, REQUEST_CODE_PICK_ROM)
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_CODE_PICK_ROM) {
            return
        }
        if (resultCode != RESULT_OK) {
            notifyPickerDismissedSafe()
            return
        }

        val uri = data?.data ?: run {
            notifyPickerDismissedSafe()
            return
        }
        try {
            val flags = data.flags and
                (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            contentResolver.takePersistableUriPermission(uri, flags)
        } catch (_: SecurityException) {
            // Non-persistable providers are still fine for immediate load.
        }

        val displayName = queryDisplayName(uri) ?: (uri.lastPathSegment ?: "picked.vb")
        val payload = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: run {
            notifyPickerDismissedSafe()
            return
        }
        if (payload.isEmpty()) {
            notifyPickerDismissedSafe()
            return
        }
        notifyRomSelectedSafe(payload, displayName)
        notifyPickerDismissedSafe()
    }

    private fun notifyRomSelectedSafe(data: ByteArray, displayName: String) {
        try {
            nativeOnRomSelected(data, displayName)
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "nativeOnRomSelected unavailable", e)
        }
    }

    private fun notifyPickerDismissedSafe() {
        try {
            nativeOnRomPickerDismissed()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "nativeOnRomPickerDismissed unavailable", e)
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        val cursor: Cursor = contentResolver.query(uri, null, null, null, null) ?: return null
        cursor.use {
            val nameIndex = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (nameIndex >= 0 && it.moveToFirst()) {
                return it.getString(nameIndex)
            }
        }
        return null
    }

    private companion object {
        init {
            try {
                System.loadLibrary("virtualvirtualboy")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed loading native library", e)
            }
        }

        const val TAG = "VRboy"
        const val REQUEST_CODE_PICK_ROM = 1001
    }
}
