package com.keitark.virtualvirtualboy

import android.app.NativeActivity
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns

class MainActivity : NativeActivity() {
    external fun nativeOnRomSelected(data: ByteArray, displayName: String)

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
        if (requestCode != REQUEST_CODE_PICK_ROM || resultCode != RESULT_OK) {
            return
        }

        val uri = data?.data ?: return
        try {
            val flags = data.flags and
                (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            contentResolver.takePersistableUriPermission(uri, flags)
        } catch (_: SecurityException) {
            // Non-persistable providers are still fine for immediate load.
        }

        val displayName = queryDisplayName(uri) ?: (uri.lastPathSegment ?: "picked.vb")
        val payload = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: return
        if (payload.isEmpty()) {
            return
        }
        nativeOnRomSelected(payload, displayName)
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
        const val REQUEST_CODE_PICK_ROM = 1001
    }
}
