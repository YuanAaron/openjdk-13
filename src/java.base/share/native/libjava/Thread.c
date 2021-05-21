/*
 * Copyright (c) 1994, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*-
 *      Stuff for dealing with threads.
 *      originally in threadruntime.c, Sun Sep 22 12:09:39 1991
 */

#include "jni.h"
#include "jvm.h"

#include "java_lang_Thread.h"

#define THD "Ljava/lang/Thread;"
#define OBJ "Ljava/lang/Object;"
#define STE "Ljava/lang/StackTraceElement;"
#define STR "Ljava/lang/String;"

#define ARRAY_LENGTH(a) (sizeof(a)/sizeof(a[0]))

/**
 * Java中start方法调用run的底层原理，主要参考：
 * https://www.jianshu.com/p/3ce1b5e5a55e
 * https://blog.csdn.net/qq_33996921/article/details/106738629
 * https://www.bilibili.com/video/BV1ki4y1A793
 *
 * Mac下使用JNI(Java Native Interface)实现Java调用C语言：https://www.cnblogs.com/chenmo-xpw/p/7501325.html
 * Mac下使用JNI实现C语言调用Java：https://blog.csdn.net/yuhentian/article/details/79945112 和 https://blog.csdn.net/TECH_PRO/article/details/70800359
 *
 * 几个概念
 * java.lang.Thread: Java语言中的线程类，由这个Java类创建的instance都会1:1映射到一个操作系统的osthread（平台级线程/内核态线程）;
 * JavaThread: JVM中C++定义的类，一个JavaThread的instance代表了在JVM中的java.lang.Thread的instance，它维护了线程的状态，并且维护
 *      了一个指向java.lang.Thread创建的对象（oop）的指针，它同时还维护了一个指向对应的OSThread的指针（获取底层操作系统创建的osthread的状态）
 * OSThread：JVM中C++定义的类，代表了JVM对底层操作系统的osthread的抽象，它维护着实际操作系统创建的线程句柄handle（可以获取底层osthread的状态）
 * VMThread: JVM中C++定义的类，这个类和用户创建的线程无关，是JVM本身用来进行虚拟机操作的系统，比如GC。
 *
 * OSThread和osthread的区别：
 * 简单的说，osthread是操作系统级别的线程，也就是真正意义上的线程，它的具体实现由操作系统完成；
 * OSThread是对osthread的抽象，通过操作系统暴露出来的接口（句柄）对osthread进行操作。在实际的运行中，OSThread会根据所运行的操作系统（平台）进行创建，
 * 即如果运行字节码的平台是Linux，那么创建出来的OSThread就是Linux的OSThread；如果是Windows的话，那么创建出来的就是Windows的OSThread。
 *
 * 结论：
 * 1、在Java中，使用java.lang.Thread的构造方法来创建一个java.lang.Thread对象，此时只是对这个对象的部分字段（比如线程名、优先级等）进行初始化；
 * 2、调用java.lang.Thread对象的start()方法，开启此线程。在start()方法内部，调用start0()本地方法来开启此线程；
 * 3、start0()在JVM中对应的是JVM_StartThread，即在JVM中，实际运行的是JVM_StartThread方法（宏），在这个方法中创建了一个JavaThread对象；
 * 4、在JavaThread创建的过程中，会根据运行平台创建一个对应的OSThread对象，且JavaThread保持这个OSThread对象的引用；
 * 5、在OSThread对象的创建过程中，创建一个osthread(平台相关的底层级别线程、内核态线程），如果这个底层级线程失败，就会抛出异常；
 * 6、在正常情况下，这个底层级别的线程开始运行，并执行java.lang.Thread对象的run方法；
 * 7、当java.lang.Thread生成的Object的run()方法执行完毕返回或抛出异常终止后，终止底层级线程；
 * 8、释放JVM相关的thread的资源，清除对应的JavaThread和OSThread。
 *
 * 在上述过程中，穿插着各种判断和检测，其中很大一部分都是关于各种层次下的线程状态的检测，因此在JVM中，无论哪种层次的线程，都只允许执行一次。
 */

//第一部分
//以下函数指针的定义在jvm.h中，实现在jvm.cpp中
static JNINativeMethod methods[] = {
    // 线程启动调用start0，而实际会执行JVM_StartThread方法（从名字上看，似乎在JVM层面去
    // 启动一个线程，在JVM层面，一定会调用Java中的run方法，继续看后面代码寻找答案）
    {"start0",           "()V",        (void *)&JVM_StartThread}, //1-private native void start0();
    {"stop0",            "(" OBJ ")V", (void *)&JVM_StopThread},
    {"isAlive",          "()Z",        (void *)&JVM_IsThreadAlive},
    {"suspend0",         "()V",        (void *)&JVM_SuspendThread},
    {"resume0",          "()V",        (void *)&JVM_ResumeThread},
    {"setPriority0",     "(I)V",       (void *)&JVM_SetThreadPriority},
    {"yield",            "()V",        (void *)&JVM_Yield},
    {"sleep",            "(J)V",       (void *)&JVM_Sleep},
    {"currentThread",    "()" THD,     (void *)&JVM_CurrentThread},
    {"countStackFrames", "()I",        (void *)&JVM_CountStackFrames},
    {"interrupt0",       "()V",        (void *)&JVM_Interrupt},
    {"isInterrupted",    "(Z)Z",       (void *)&JVM_IsInterrupted},
    {"holdsLock",        "(" OBJ ")Z", (void *)&JVM_HoldsLock},
    {"getThreads",        "()[" THD,   (void *)&JVM_GetAllThreads},
    {"dumpThreads",      "([" THD ")[[" STE, (void *)&JVM_DumpThreads},
    {"setNativeName",    "(" STR ")V", (void *)&JVM_SetNativeThreadName},
};

#undef THD
#undef OBJ
#undef STE
#undef STR

JNIEXPORT void JNICALL
Java_java_lang_Thread_registerNatives(JNIEnv *env, jclass cls)
{
    (*env)->RegisterNatives(env, cls, methods, ARRAY_LENGTH(methods));
}
