#!/usr/bin/env python

import sys
from launcher import Launcher

hostfile = "machinefiles/5nodes"
progfile = "debug/LoadExample"
schedulerfile = "debug/SchedulerMain"

common_params = {
    "scheduler" : "proj10",
    "scheduler_port" : "33254",
    "hdfs_namenode" : "proj10",
    "hdfs_port" : 9000,
}

program_params = {
    # "url" : "/datasets/classification/kdd12-5blocks",
    "url" : "/tmp/tmp/banana.txt",
    "output_url" : "/tmp/tmp/res/",
    "num_local_threads" : 2,
}

scheduler_params = {
}

env_params = (
  "GLOG_logtostderr=true "
  "GLOG_v=-1 "
  "GLOG_minloglevel=0 "
  # this is to enable hdfs short-circuit read (disable the warning info)
  # change this path accordingly when we use other cluster
  # the current setting is for proj5-10
  "LIBHDFS3_CONF=/data/opt/course/hadoop/etc/hadoop/hdfs-site.xml"
  )

l = Launcher(schedulerfile, progfile, hostfile,
             common_params, scheduler_params, program_params, env_params,
             dump_core=False)

l.Launch(sys.argv)
# l.DebugString()

