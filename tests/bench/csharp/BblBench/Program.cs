using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;

class Program
{
    static long LoopArith()
    {
        long n = 10000000, sum = 0;
        for (long i = 0; i < n; i++) sum += i;
        return sum;
    }

    static long Add(long a, long b) => a + b;
    static long FunctionCalls()
    {
        long sum = 0;
        for (long i = 0; i < 5000000; i++) sum = Add(sum, i);
        return sum;
    }

    static long GcPressure()
    {
        long i = 0;
        for (; i < 1000000; i++)
        {
            var t = new Dictionary<string, long> { { "k", i } };
        }
        return i;
    }

    static int StringIntern()
    {
        var sb = new StringBuilder();
        for (int i = 0; i < 1000000; i++) sb.Append('x');
        return sb.Length;
    }

    static long TableHeavy()
    {
        var t = new Dictionary<long, long>();
        for (long i = 0; i < 100000; i++) t[i] = i * i;
        long sum = 0;
        for (long i = 0; i < 100000; i++) sum += t[i];
        return sum;
    }

    static long Fib(long n) => n <= 1 ? n : Fib(n - 1) + Fib(n - 2);
    static long Recursion() => Fib(35);

    static long ClosureCapture()
    {
        var makers = new List<Func<long>>();
        for (long i = 0; i < 10000; i++)
        {
            long x = i, y = i * 2;
            makers.Add(() => x + y);
        }
        long sum = 0;
        for (int i = 0; i < 10000; i++) sum += makers[i]();
        return sum;
    }

    static long MethodDispatch()
    {
        var v = new long[] { 1, 2, 3, 4, 5 };
        var t = new Dictionary<string, long> { { "a", 1 }, { "b", 2 }, { "c", 3 } };
        string s = "hello world";
        string[] keys = { "a", "b", "c" };
        long sum = 0;
        for (int i = 0; i < 100000; i++)
        {
            sum += v[i % 5];
            sum += t[keys[i % 3]];
            sum += s.Length;
        }
        return sum;
    }

    static void Main(string[] args)
    {
        if (args.Length == 0) { Console.WriteLine("Usage: BblBench <benchmark>"); return; }
        switch (args[0])
        {
            case "loop_arith": Console.WriteLine(LoopArith()); break;
            case "function_calls": Console.WriteLine(FunctionCalls()); break;
            case "gc_pressure": Console.WriteLine(GcPressure()); break;
            case "string_intern": Console.WriteLine(StringIntern()); break;
            case "table_heavy": Console.WriteLine(TableHeavy()); break;
            case "recursion": Console.WriteLine(Recursion()); break;
            case "closure_capture": Console.WriteLine(ClosureCapture()); break;
            case "method_dispatch": Console.WriteLine(MethodDispatch()); break;
        }
    }
}
