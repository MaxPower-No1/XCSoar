/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009

	M Roberts (original release)
	Robin Birch <robinb@ruffnready.co.uk>
	Samuel Gisiger <samuel.gisiger@triadis.ch>
	Jeff Goodenough <jeff@enborne.f2s.com>
	Alastair Harrison <aharrison@magic.force9.co.uk>
	Scott Penrose <scottp@dd.com.au>
	John Wharington <jwharington@gmail.com>
	Lars H <lars_hn@hotmail.com>
	Rob Dunning <rob@raspberryridgesheepfarm.com>
	Russell King <rmk@arm.linux.org.uk>
	Paolo Ventafridda <coolwind@email.it>
	Tobias Lohner <tobias@lohner-net.de>
	Mirek Jezek <mjezek@ipplc.cz>
	Max Kellermann <max@duempel.org>
	Tobias Bieniek <tobias.bieniek@gmx.de>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}

*/

#include "ConditionMonitor.hpp"
#include "Math/Geometry.hpp"
#include "Message.hpp"
#include "Device/device.hpp"
#include "Protection.hpp"
#include "Math/SunEphemeris.hpp"
#include "LocalTime.hpp"
#include "InputEvents.h"
#include "Math/SunEphemeris.hpp"
#include "GlideComputer.hpp"
#include "Components.hpp"

#ifdef OLD_TASK
#include "WayPoint.hpp"
#include "WayPointList.hpp"
#endif

#include <math.h>

// TODO: JMW: make this use GPSClock (code re-use)

class ConditionMonitor
{
public:
  ConditionMonitor()
  {
    LastTime_Notification = -1;
    LastTime_Check = -1;
  }

  void
  Update(const GlideComputer& cmp)
  {
    if (!cmp.Basic().flight.Flying)
      return;

    bool restart = false;
    const fixed Time = cmp.Basic().Time;
    if (Ready_Time_Check(Time, &restart)) {
      LastTime_Check = Time;
      if (CheckCondition(cmp)) {
        if (Ready_Time_Notification(Time) && !restart) {
          LastTime_Notification = Time;
          Notify();
          SaveLast();
        }
      }

      if (restart)
        SaveLast();
    }
  }

protected:
  double LastTime_Notification;
  double LastTime_Check;
  double Interval_Notification;
  double Interval_Check;

private:

  virtual bool CheckCondition(const GlideComputer& cmp) = 0;
  virtual void Notify(void) = 0;
  virtual void SaveLast(void) = 0;

  bool
  Ready_Time_Notification(double T)
  {
    if (T <= 0)
      return false;

    if ((T < LastTime_Notification) || (LastTime_Notification == -1))
      return true;

    if (T >= LastTime_Notification + Interval_Notification)
      return true;

    return false;
  }

  bool
  Ready_Time_Check(double T, bool *restart)
  {
    if (T <= 0)
      return false;

    if ((T < LastTime_Check) || (LastTime_Check == -1)) {
      LastTime_Notification = -1;
      *restart = true;
      return true;
    }

    if (T >= LastTime_Check + Interval_Check)
      return true;

    return false;
  }
};

class ConditionMonitorWind: public ConditionMonitor
{
public:
  ConditionMonitorWind()
  {
    Interval_Notification = 60 * 5;
    Interval_Check = 10;
  }

protected:
  bool
  CheckCondition(const GlideComputer& cmp)
  {
    wind = cmp.Basic().wind;

    if (!cmp.Basic().flight.Flying) {
      last_wind = wind;
      return false;
    }

    fixed mag_change = fabs(wind.norm - last_wind.norm);
    fixed dir_change = fabs(AngleLimit180(wind.bearing - last_wind.bearing));

    if (mag_change > Units::ToSysUnit(5, unKnots))
      return true;

    if ((wind.norm > Units::ToSysUnit(10, unKnots)) && (dir_change > 45))
      return true;

    return false;
  }

  void
  Notify(void)
  {
    Message::AddMessage(TEXT("Significant wind change"));
  }

  void
  SaveLast(void)
  {
    last_wind = wind;
  }

private:
  SpeedVector wind;
  SpeedVector last_wind;
};

class ConditionMonitorFinalGlide: public ConditionMonitor
{
public:
  ConditionMonitorFinalGlide()
  {
    Interval_Notification = 60 * 5;
    Interval_Check = 1;
    tad = 0;
  }

protected:
  bool
  CheckCondition(const GlideComputer& cmp)
  {
#ifdef OLD_TASK    
    if (!cmp.Basic().Flying || !task.Valid())
      return false;

    // TODO: use low pass filter
    tad = cmp.Calculated().TaskAltitudeDifference * 0.2 + 0.8 * tad;

    bool BeforeFinalGlide = (task.ValidTaskPoint(task.getActiveIndex() + 1)
                            && !cmp.Calculated().FinalGlide);

    if (BeforeFinalGlide) {
      Interval_Notification = 60 * 5;
      if ((tad > 50) && (last_tad < -50))
        // report above final glide early
        return true;
      else if (tad < -50)
        last_tad = tad;
    } else {
      Interval_Notification = 60;
      if (cmp.Calculated().FinalGlide) {
        if ((last_tad < -50) && (tad > 1))
          // just reached final glide, previously well below
          return true;

        if ((last_tad > 1) && (tad < -50)) {
          // dropped well below final glide, previously above
          last_tad = tad;
          return true; // JMW this was true before
        }
      }
    }
#endif
    return false;
  }

  void
  Notify(void)
  {
    if (tad > 1)
      InputEvents::processGlideComputer(GCE_FLIGHTMODE_FINALGLIDE_ABOVE);
    if (tad < -1)
      InputEvents::processGlideComputer(GCE_FLIGHTMODE_FINALGLIDE_BELOW);
  }

  void
  SaveLast(void)
  {
    last_tad = tad;
  }

private:
  double tad;
  double last_tad;
};

class ConditionMonitorSunset: public ConditionMonitor
{
public:
  ConditionMonitorSunset()
  {
    Interval_Notification = 60 * 30;
    Interval_Check = 60;
  }
protected:
  SunEphemeris sun;

  bool
  CheckCondition(const GlideComputer& cmp)
  {
#ifdef OLD_TASK
    if (!task.Valid() || !cmp.Basic().Flying)
      return false;

    double sunsettime = sun.CalcSunTimes(task.getActiveLocation(),
        cmp.Basic().DateTime, GetUTCOffset() / 3600);
    double d1 = (cmp.Calculated().TaskTimeToGo
        + DetectCurrentTime(&cmp.Basic())) / 3600;
    double d0 = (DetectCurrentTime(&cmp.Basic())) / 3600;

    bool past_sunset = (d1 > sunsettime) && (d0 < sunsettime);

    if (past_sunset && !HaveCondorDevice())
      // notify on change only
      return true;
    else
      return false;
    }
#else
    return false;
#endif
  }

  void
  Notify(void)
  {
    Message::AddMessage(TEXT("Expect arrival past sunset"));
  }

  void
  SaveLast(void)
  {
  }

private:
};

class ConditionMonitorAATTime: public ConditionMonitor
{
public:
  ConditionMonitorAATTime()
  {
    Interval_Notification = 60 * 15;
    Interval_Check = 10;
  }

protected:
  bool
  CheckCondition(const GlideComputer& cmp)
  {
#ifdef OLD_TASK
    if (!task.getSettings().AATEnabled || !task.Valid()
        || task.TaskIsTemporary() || !(cmp.Calculated().ValidStart
        && !cmp.Calculated().ValidFinish) || !cmp.Basic().Flying)
      return false;

    bool OnFinalWaypoint = !task.Valid();
    if (OnFinalWaypoint)
      // can't do much about it now, so don't give a warning
      return false;

    if (cmp.Calculated().TaskTimeToGo < cmp.Calculated().AATTimeToGo)
      return true;
    else
      return false;
    }
#else
    return false;
#endif
  }

  void
  Notify(void)
  {
    Message::AddMessage(TEXT("Expect early task arrival"));
  }

  void
  SaveLast(void)
  {
  }

private:
};

class ConditionMonitorStartRules: public ConditionMonitor
{
public:
  ConditionMonitorStartRules()
  {
    Interval_Notification = 60;
    Interval_Check = 1;
    withinMargin = false;
  }
protected:

  bool
  CheckCondition(const GlideComputer& cmp)
  {
#ifdef OLD_TASK
    if (!task.Valid()
        || !cmp.Basic().Flying
        || (task.getActiveIndex() > 0)
        || !task.ValidTaskPoint(task.getActiveIndex() + 1))
      return false;

    if (cmp.Calculated().LegDistanceToGo > task.getSettings().StartRadius)
      return false;

    if (cmp.ValidStartSpeed(task.getSettings().StartMaxSpeedMargin)
        && cmp.InsideStartHeight(task.getSettings().StartMaxHeightMargin))
      withinMargin = true;
    else
      withinMargin = false;
    }
    return !(cmp.ValidStartSpeed() && cmp.InsideStartHeight());
#else
    return false;
#endif
  }

  void
  Notify(void)
  {
    if (withinMargin)
      Message::AddMessage(TEXT("Start rules slightly violated\r\nbut within margin"));
    else
      Message::AddMessage(TEXT("Start rules violated"));
  }

  void
  SaveLast(void)
  {
  }

private:
  bool withinMargin;
};

class ConditionMonitorGlideTerrain: public ConditionMonitor
{
public:
  ConditionMonitorGlideTerrain()
  {
    Interval_Notification = 60 * 5;
    Interval_Check = 1;
    fgtt = 0;
    fgtt_last = false;
  }

protected:
  bool
  CheckCondition(const GlideComputer& cmp)
  {
#ifdef OLD_TASK
    if (!cmp.Basic().Flying || !task.Valid())
      return false;

    fgtt = !((cmp.Calculated().TerrainWarningLocation.Latitude == 0.0)
        && (cmp.Calculated().TerrainWarningLocation.Longitude == 0.0));

    if (!cmp.Calculated().FinalGlide
        || (cmp.Calculated().TaskAltitudeDifference < -50))
      fgtt_last = false;
    else if ((fgtt) && (!fgtt_last))
      // just reached final glide, previously well below
      return true;
    }
#endif
    return false;
  }

  void
  Notify(void)
  {
    InputEvents::processGlideComputer(GCE_FLIGHTMODE_FINALGLIDE_TERRAIN);
  }

  void
  SaveLast(void)
  {
    fgtt_last = fgtt;
  }

private:
  bool fgtt;
  bool fgtt_last;
};

ConditionMonitorWind cm_wind;
ConditionMonitorFinalGlide cm_finalglide;
ConditionMonitorSunset cm_sunset;
ConditionMonitorAATTime cm_aattime;
ConditionMonitorStartRules cm_startrules;
ConditionMonitorGlideTerrain cm_glideterrain;

void
ConditionMonitorsUpdate(const GlideComputer& cmp)
{
  cm_wind.Update(cmp);
  cm_finalglide.Update(cmp);
  cm_sunset.Update(cmp);
  cm_aattime.Update(cmp);
  cm_startrules.Update(cmp);
  cm_glideterrain.Update(cmp);
}
