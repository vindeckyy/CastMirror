using System;
using System.Threading;

namespace CastMirror.Services
{
    /// <summary>
    /// Enforces one CastMirror per user session.
    ///
    /// The engine keeps its config, discovery listeners, and Cast session in
    /// process-wide native state, so a second launch would open a second
    /// discovery listener on the same LAN and could start a competing cast.
    /// Closing to tray already keeps the process alive in the common case, so a
    /// second copy only appears when the user launches it again deliberately.
    ///
    /// The name is local to the session, so it neither collides across users nor
    /// with an instance running in another session.
    /// </summary>
    public static class SingleInstance
    {
        private const string MutexName = @"Local\CastMirror.SingleInstance";

        private static Mutex? _mutex;
        private static bool _isPrimary;

        /// <summary>
        /// Attempts to claim the single-instance slot.
        /// </summary>
        /// <returns>
        /// True when this process owns the slot and should continue starting up;
        /// false when another instance already holds it and this one should exit.
        /// </returns>
        public static bool TryAcquire()
        {
            try
            {
                // createdNew is true only for the process that creates the named
                // object, which is exactly the "am I the first?" answer.
                _mutex = new Mutex(initiallyOwned: true, MutexName, out bool createdNew);
                if (createdNew)
                {
                    _isPrimary = true;
                    return true;
                }
                _mutex.Dispose();
                _mutex = null;
                return false;
            }
            catch (Exception ex)
            {
                // If the mutex cannot be created at all (locked-down profile, a
                // leftover object), launching beats refusing to run at all.
                LogService.Log($"[single-instance] guard unavailable, continuing: {ex.Message}");
                _isPrimary = true;
                return true;
            }
        }

        /// <summary>True when this process owns the single-instance slot.</summary>
        public static bool IsPrimary => _isPrimary;

        /// <summary>Releases the slot. Safe to call more than once.</summary>
        public static void Release()
        {
            try
            {
                _mutex?.ReleaseMutex();
            }
            catch (ApplicationException)
            {
                // Not owned on this thread; the process is exiting regardless.
            }
            catch (Exception ex)
            {
                LogService.Log($"[single-instance] release failed: {ex.Message}");
            }
            finally
            {
                _mutex?.Dispose();
                _mutex = null;
            }
        }
    }
}