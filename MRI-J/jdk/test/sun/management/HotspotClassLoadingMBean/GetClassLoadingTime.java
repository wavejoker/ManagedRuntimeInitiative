/*
 * Copyright 2003 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * @test
 * @bug     4858522
 * @summary Basic unit test of HotspotClassLoadingMBean.getClassLoadingTime()
 * @author  Steve Bohne
 * @build ClassToLoad0
 */

/*
 * This test is just a sanity check and does not check for the correct value.
 */

import java.io.*;
import sun.management.*;

public class GetClassLoadingTime {

    private static HotspotClassLoadingMBean mbean =
        (HotspotClassLoadingMBean)ManagementFactory.getHotspotClassLoadingMBean();

    // Careful with these values.
    private static final long MIN_TIME_FOR_PASS = 1;
    private static final long MAX_TIME_FOR_PASS = Long.MAX_VALUE;

    private static boolean trace = false;

    public static void main(String args[]) throws Exception {
        if (args.length > 0 && args[0].equals("trace")) {
            trace = true;
        }

        long time = mbean.getClassLoadingTime();

        if (trace) {
            System.out.println("Class loading time (ms): " + time);
        }

        if (time < MIN_TIME_FOR_PASS || time > MAX_TIME_FOR_PASS) {
            throw new RuntimeException("Class loading time " +
                                       "illegal value: " + time + " ms " +
                                       "(MIN = " + MIN_TIME_FOR_PASS + "; " +
                                       "MAX = " + MAX_TIME_FOR_PASS + ")");
        }

        // Load some classes to increase the time
        for (int i = 0; i < 1000; i++) {
            Class.forName("ClassToLoad0", true, new KlassLoader());
        }

        long time2 = mbean.getClassLoadingTime();
        long count = mbean.getLoadedClassCount();

        if (trace) {
            System.out.println("(new count is " + count + ")");
            System.out.println("Class loading time2 (ms): " + time2);
        }

        if (time2 <= time) {
            throw new RuntimeException("Class loading time " +
                                       "did not increase when class loaded" +
                                       "(time = " + time + "; " +
                                       "time2 = " + time2 + ")");
        }

        System.out.println("Test passed.");
    }
}

// KlassLoader exists to load classes without a parent classloader,
// so we can avoid delegation and spend lots of time loading the
// same class over and over, to test the class loading timer.
class KlassLoader extends ClassLoader {
  static String klassDir="";
  static int index=0;

  public KlassLoader() {
      super(null);
  }

  protected synchronized Class findClass(String name)
                        throws ClassNotFoundException {
        String cname = klassDir
            + (klassDir == "" ? "" : "/")
            +name.replace('.', '/')
            +".class";

        FileInputStream in;
        try {
                in=new FileInputStream(cname);
                if (in == null) {
                        throw new ClassNotFoundException("getResourceAsStream("
                                +cname+")");
                }
        } catch(java.io.FileNotFoundException e ) {
                throw new ClassNotFoundException("getResourceAsStream("
                        +cname+") : "
                        +e);
        }

        int len;
        byte data[];
        try {
                len = in.available();
                data = new byte[len];
                for (int total = 0; total < data.length; ) {
                        total += in.read(data, total, data.length - total);
                }
        } catch (IOException e) {
                throw new ClassNotFoundException(cname, e);
        } finally {
                try {
                        in.close();
                } catch (IOException e) {
                }
        }

        return defineClass(name, data, 0, data.length);
  }
}
