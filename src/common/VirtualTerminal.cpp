/***************************************************************************
* Copyright (c) 2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include <QDebug>
#include <QString>

#include "VirtualTerminal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <qscopeguard.h>

#define RELEASE_DISPLAY_SIGNAL (SIGRTMAX)
#define ACQUIRE_DISPLAY_SIGNAL (SIGRTMAX - 1)

namespace SDDM {
    namespace VirtualTerminal {
        static void onAcquireDisplay(int signal) {
            int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
            ioctl(fd, VT_RELDISP, VT_ACKACQ);
            close(fd);
        }

        static void onReleaseDisplay(int signal) {
            int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
            ioctl(fd, VT_RELDISP, 1);
            close(fd);
        }

        static bool handleVtSwitches(int fd) {
            vt_mode setModeRequest = { 0 };
            bool ok = true;

            setModeRequest.mode = VT_PROCESS;
            setModeRequest.relsig = RELEASE_DISPLAY_SIGNAL;
            setModeRequest.acqsig = ACQUIRE_DISPLAY_SIGNAL;

            if (ioctl(fd, VT_SETMODE, &setModeRequest) < 0) {
                qDebug() << "Failed to manage VT manually:" << strerror(errno);
                ok = false;
            }

            signal(RELEASE_DISPLAY_SIGNAL, onReleaseDisplay);
            signal(ACQUIRE_DISPLAY_SIGNAL, onAcquireDisplay);

            return ok;
        }

        static void fixVtMode(int fd, bool vt_auto) {
            vt_mode getmodeReply = { 0 };
            int kernelDisplayMode = 0;
            bool modeFixed = false;
            bool ok = true;

            if (ioctl(fd, VT_GETMODE, &getmodeReply) < 0) {
                qWarning() << "Failed to query VT mode:" << strerror(errno);
                ok = false;
            }

            if (getmodeReply.mode != VT_AUTO)
                goto out;

            if (ioctl(fd, KDGETMODE, &kernelDisplayMode) < 0) {
                qWarning() << "Failed to query kernel display mode:" << strerror(errno);
                ok = false;
            }

            if (kernelDisplayMode == KD_TEXT)
                goto out;

            // VT is in the VT_AUTO + KD_GRAPHICS state, fix it
            if (vt_auto) {
                // If vt_auto is true, the controlling process is already gone, so there is no
                // process which could send the VT_RELDISP 1 ioctl to release the vt.
                // Switch to KD_TEXT and let the kernel switch vts automatically
                if (ioctl(fd, KDSETMODE, KD_TEXT) < 0) {
                    qWarning("Failed to set text mode for current VT: %s", strerror(errno));
                    ok = false;
                }
            }
            else {
                ok = handleVtSwitches(fd);
                modeFixed = true;
            }
out:
            if (!ok) {
                qCritical() << "Failed to set up VT mode";
                return;
            }

            if (modeFixed)
                qDebug() << "VT mode fixed";
            else
                qDebug() << "VT mode didn't need to be fixed";
        }

        int fetchAvailableVt() {
            // open VT master
            int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
            if (fd < 0) {
                qCritical() << "Failed to open VT master:" << strerror(errno);
                return -1;
            }
            auto closeFd = qScopeGuard([fd] {
                close(fd);
            });

            vt_stat vtState = { 0 };
            if (ioctl(fd, VT_GETSTATE, &vtState) < 0) {
                qCritical() << "Failed to get current VT:" << strerror(errno);

                int vt = 0;
                // If there's no current tty, request the next to open
                if (ioctl(fd, VT_OPENQRY, &vt) < 0) {
                    qCritical() << "Failed to open new VT:" << strerror(errno);
                    return -1;
                }
                return vt;
            }
            return vtState.v_active;
        }

        int setUpNewVt() {
            // open VT master
            int fd = open("/dev/tty0", O_RDWR | O_NOCTTY);
            if (fd < 0) {
                qCritical() << "Failed to open VT master:" << strerror(errno);
                return -1;
            }
            auto closeFd = qScopeGuard([fd] {
                close(fd);
            });

            int vt = 0;
            if (ioctl(fd, VT_OPENQRY, &vt) < 0) {
                qCritical() << "Failed to open new VT:" << strerror(errno);
                return -1;
            }

            // fallback to active VT
            if (vt <= 0) {
                vt_stat vtState = { 0 };
                if (ioctl(fd, VT_GETSTATE, &vtState) < 0) {
                    qCritical() << "Failed to get current VT:" << strerror(errno);
                    return -1;
                }

                qWarning() << "New VT" << vt << "is not valid, fall back to" << vtState.v_active;
                return vtState.v_active;
            }

            return vt;
        }

        void jumpToVt(int vt, bool vt_auto) {
            qDebug() << "Jumping to VT" << vt;

            int fd;

            int activeVtFd = open("/dev/tty0", O_RDWR | O_NOCTTY);

            QString ttyString = QStringLiteral("/dev/tty%1").arg(vt);
            int vtFd = open(qPrintable(ttyString), O_RDWR | O_NOCTTY);
            if (vtFd != -1) {
                fd = vtFd;

                // Clear VT
                static const char *clearEscapeSequence = "\33[H\33[2J";
                write(vtFd, clearEscapeSequence, sizeof(clearEscapeSequence));

                // set graphics mode to prevent flickering
                if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0)
                    qWarning("Failed to set graphics mode for VT %d: %s", vt, strerror(errno));

                // it's possible that the current VT was left in a broken
                // combination of states (KD_GRAPHICS with VT_AUTO) that we
                // cannot switch from, so make sure things are in a way that
                // will make VT_ACTIVATE work without hanging VT_WAITACTIVE
                fixVtMode(activeVtFd, vt_auto);
            } else {
                qWarning("Failed to open %s: %s", qPrintable(ttyString), strerror(errno));
                qDebug("Using /dev/tty0 instead of %s!", qPrintable(ttyString));
                fd = activeVtFd;
            }

            // If vt_auto is true, the controlling process is already gone, so there is no
            // process which could send the VT_RELDISP 1 ioctl to release the vt.
            // Let the kernel switch vts automatically
            if (!vt_auto)
                handleVtSwitches(fd);

            do {
                errno = 0;

                if (ioctl(fd, VT_ACTIVATE, vt) < 0) {
                    if (errno == EINTR)
                        continue;

                    qWarning("Couldn't initiate jump to VT %d: %s", vt, strerror(errno));
                    break;
                }

                if (ioctl(fd, VT_WAITACTIVE, vt) < 0 && errno != EINTR)
                    qWarning("Couldn't finalize jump to VT %d: %s", vt, strerror(errno));

            } while (errno == EINTR);
            close(activeVtFd);
            if (vtFd != -1)
                close(vtFd);
        }
    }
}
