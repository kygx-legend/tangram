#!/usr/bin/env python

import sys
from launcher import Launcher

hostfile = "machinefiles/5nodes"
progfile = "debug/PageRankWith"
schedulerfile = "debug/SchedulerMain"

common_params = {
    "scheduler" : "proj10",
    "scheduler_port" : "33224",
    "hdfs_namenode" : "proj10",
    "hdfs_port" : 9000,
}

program_params = {
    "url" : "/datasets/graph/google-adj",
    "num_local_threads" : 20,
    "num_parts" : 100,
    "combine_type": "kDirectCombine",
}

scheduler_params = {
    "dag_runner_type" : "sequential",
}

env_params = (
  "GLOG_logtostderr=true "
  "GLOG_v=-1 "
  "GLOG_minloglevel=0 "
  # this is to enable hdfs short-circuit read (disable the warning info)
  # change this path accordingly when we use other cluster
  # the current setting is for proj5-10
  "LIBHDFS3_CONF=/data/opt/course/hadoop/etc/hadoop/hdfs-site.xml"

  # turn on this to run in w1-20
  # "LIBHDFS3_CONF=/data/opt/hadoop-2.6.0/etc/hadoop/hdfs-site.xml"
  )

dump_core = False
l = Launcher(schedulerfile, progfile, hostfile,
             common_params, scheduler_params, program_params, env_params,
             dump_core)

l.Launch(sys.argv)
