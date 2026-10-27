package com.resonate.engine;

import android.app.NativeActivity;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

import java.nio.charset.StandardCharsets;

/**
 * A NativeActivity with one invisible EditText that owns the IME connection.
 *
 * Android delivers composed text (pinyin, kana, ...) to a View's
 * InputConnection and never to the native window, so CJK input needs a real
 * text editor on the Java side. The field stays 1x1 and fully transparent; the
 * engine draws the text itself and mirrors what the editor holds. The engine
 * shows and hides the soft keyboard as text widgets take and lose focus.
 */
public class ResonateActivity extends NativeActivity {
    static {
        /* Associates the host library with this class loader so the class's
           native methods resolve; the framework loads the same library again
           for ANativeActivity_onCreate, which the runtime reference-counts. */
        System.loadLibrary("main");
    }

    private EditText field;

    /** The editor's whole text, sent whenever it changes (committed + composing). */
    private static native void nativeTextChanged(byte[] text);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        field = new EditText(this);
        field.setAlpha(0.0f);
        field.setBackground(null);
        field.setSingleLine(true);
        field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        addContentView(field,
                new FrameLayout.LayoutParams(1, 1, Gravity.TOP | Gravity.LEFT));
        field.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable text) {
                nativeTextChanged(text.toString().getBytes(StandardCharsets.UTF_8));
            }
        });
        field.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_DONE) {
                hideKeyboard();
                return true;
            }
            return false;
        });
    }

    /** Replaces the editor's buffer; called from native as a field takes focus. */
    public void setEditorBuffer(byte[] text) {
        final String value = new String(text, StandardCharsets.UTF_8);
        runOnUiThread(() -> {
            field.setText(value);
            field.setSelection(field.getText().length());
        });
    }

    /** Shows the soft keyboard; called from native when a text widget gains focus. */
    public void showKeyboard() {
        runOnUiThread(() -> {
            field.requestFocus();
            /* showSoftInput right after requestFocus is dropped when the
               window has not taken input focus yet. */
            field.postDelayed(() -> {
                InputMethodManager manager = getSystemService(InputMethodManager.class);
                if (manager != null) {
                    manager.showSoftInput(field, InputMethodManager.SHOW_IMPLICIT);
                }
            }, 120);
        });
    }

    /** Hides the soft keyboard; called from native when focus leaves a text widget. */
    public void hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager manager = getSystemService(InputMethodManager.class);
            if (manager != null) {
                manager.hideSoftInputFromWindow(field.getWindowToken(), 0);
            }
            field.clearFocus();
        });
    }
}
