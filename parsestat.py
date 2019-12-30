#!/usr/bin/python3
import sys
import pandas as pd
import plotly.express as px
import plotly.graph_objects as go
from plotly.subplots import make_subplots

context = None
arguments = None

def setContext(line):
    global context
    context = line.rstrip()

def setArguments(line):
    global arguments
    arguments = line.rstrip().split(':')[1].split(", ")

switcher = {
    "app": setContext,
    "args": setArguments}

def readFile(stats):
    arr = []
    for line in stats:
        if line[0] == '#':
            switcher[line[1:].split(':')[0]](line[1:])
            continue
        arr.append([int(x) for x in line.split(',')])
    #return list(map(list, zip(*arr)))
    return arr

def createLinePlot(df):
    fig = px.line(df, x='time(s)', y="rtpsrc-bitrate(kbit/s)", title='bitrate')
    fig.show()

def createDoublePlot(df):
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    fig.add_trace(
        go.Scatter(y=df['bytes-received'], x=df['time(s)'], name="bytes-received"),
        secondary_y=False,)
    fig.add_trace(
        go.Scatter(y=df['rtpsrc-bitrate(kbit/s)'], x=df['time(s)'], name="bitrate"),
        secondary_y=True,)
    fig.update_layout(title_text="Bitrate vs. Received Packets")
    fig.update_xaxes(title_text="Time (s)")
    fig.update_yaxes(title_text="Kilobytes Received", secondary_y=False)
    fig.update_yaxes(title_text="Bitrate", secondary_y=True)
    fig.show()

def cleanData(df):
    # divide byte columnes by 1000 to get kilobytes
    df["bytes-received"] = df["bytes-received"].apply(lambda x: (x / 1000) * 8)

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 parsestat.py statfile")
        return 0

    f = open(sys.argv[1], "r")
    data = readFile(f)
    time = [x for x in range(0, len(data))]
    df = pd.DataFrame(data, columns = arguments)
    df['time(s)'] = time
    cleanData(df)
    createDoublePlot(df)
    #print(arr)
    #print(context)
    #print(arguments)

main()