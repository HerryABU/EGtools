using System;
using System.IO;
using System.Text;

namespace EGtools.Core
{
    // A TextWriter that forwards complete lines to a callback so the existing
    // console-based engine code (PdfExtractor / ExcelTools) can report progress
    // to any front-end (GUI, CLI, ...) without knowing about it. This is the
    // seam that keeps the back-end fully decoupled from the UI.
    public sealed class ForwardingWriter : TextWriter
    {
        private readonly Action<string> _onLine;
        private readonly StringBuilder _buf = new StringBuilder();
        private readonly object _lock = new object();

        public ForwardingWriter(Action<string> onLine) { _onLine = onLine; }

        public override Encoding Encoding => Encoding.UTF8;

        public override void Write(char value)
        {
            lock (_lock)
            {
                if (value == '\n')
                {
                    FlushLine();
                }
                else if (value == '\r')
                {
                    // ignore; handled by \n
                }
                else
                {
                    _buf.Append(value);
                }
            }
        }

        public override void Write(string? value)
        {
            if (value == null) return;
            lock (_lock)
            {
                // split on newlines to emit clean line events
                int start = 0;
                for (int i = 0; i < value.Length; i++)
                {
                    if (value[i] == '\n')
                    {
                        _buf.Append(value, start, i - start);
                        FlushLine();
                        start = i + 1;
                    }
                    else if (value[i] == '\r')
                    {
                        _buf.Append(value, start, i - start);
                        start = i + 1;
                    }
                }
                _buf.Append(value, start, value.Length - start);
            }
        }

        private void FlushLine()
        {
            var line = _buf.ToString();
            _buf.Clear();
            _onLine?.Invoke(line);
        }

        public override void Flush()
        {
            lock (_lock)
            {
                if (_buf.Length > 0) FlushLine();
            }
        }
    }
}
