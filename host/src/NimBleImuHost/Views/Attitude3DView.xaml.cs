using System.Windows.Media;
using System.Windows.Media.Media3D;
using NimBleImuHost.Protocol;

namespace NimBleImuHost.Views;

/// <summary>
/// Pure WPF Viewport3D attitude indicator. Sensor: yaw->Y, pitch->-X, roll->Z.
/// </summary>
public partial class Attitude3DView : System.Windows.Controls.UserControl
{
    private readonly ModelVisual3D _root = new();
    private readonly AxisAngleRotation3D _yawRot = new(new Vector3D(0, 1, 0), 0);
    private readonly AxisAngleRotation3D _pitchRot = new(new Vector3D(1, 0, 0), 0);
    private readonly AxisAngleRotation3D _rollRot = new(new Vector3D(0, 0, 1), 0);

    public Attitude3DView()
    {
        InitializeComponent();
        BuildScene();
    }

    public void SetAttitude(AttitudePacket packet)
    {
        _rollRot.Angle = packet.Roll;
        _pitchRot.Angle = -packet.Pitch;
        _yawRot.Angle = packet.Yaw;
    }

    private void BuildScene()
    {
        Viewport.Camera = new PerspectiveCamera
        {
            Position = new Point3D(2.2, 1.6, 2.8),
            LookDirection = new Vector3D(-2.2, -1.4, -2.8),
            UpDirection = new Vector3D(0, 1, 0),
            FieldOfView = 45,
        };

        _root.Children.Add(new ModelVisual3D { Content = new DirectionalLight(Colors.White, new Vector3D(-1, -2, -1)) });
        _root.Children.Add(new ModelVisual3D { Content = new AmbientLight(Color.FromRgb(90, 90, 100)) });

        var board = new GeometryModel3D
        {
            Geometry = BuildBox(1.2, 0.08, 0.8),
            Material = MaterialFrom(Color.FromRgb(30, 120, 80)),
            BackMaterial = MaterialFrom(Color.FromRgb(20, 80, 55)),
        };

        var nose = new GeometryModel3D
        {
            Geometry = BuildBox(0.2, 0.02, 0.15),
            Material = MaterialFrom(Colors.OrangeRed),
            Transform = new TranslateTransform3D(0, 0.06, 0.35),
        };

        var tg = new Transform3DGroup();
        tg.Children.Add(new RotateTransform3D(_rollRot));
        tg.Children.Add(new RotateTransform3D(_pitchRot));
        tg.Children.Add(new RotateTransform3D(_yawRot));

        var mg = new Model3DGroup();
        mg.Children.Add(board);
        mg.Children.Add(nose);
        _root.Children.Add(new ModelVisual3D { Content = mg, Transform = tg });

        _root.Children.Add(AxisLine(new Point3D(0, 0, 0), new Vector3D(1, 0, 0), Colors.Tomato, 0.9));
        _root.Children.Add(AxisLine(new Point3D(0, 0, 0), new Vector3D(0, 1, 0), Colors.LimeGreen, 0.9));
        _root.Children.Add(AxisLine(new Point3D(0, 0, 0), new Vector3D(0, 0, 1), Colors.DodgerBlue, 0.9));

        Viewport.Children.Add(_root);
    }

    private static Material MaterialFrom(Color color) => new DiffuseMaterial(new SolidColorBrush(color));

    private static MeshGeometry3D BuildBox(double sizeX, double sizeY, double sizeZ)
    {
        double x = sizeX / 2, y = sizeY / 2, z = sizeZ / 2;
        var mesh = new MeshGeometry3D();
        Point3D[] p = {
            new(-x, -y, -z), new(x, -y, -z), new(x, y, -z), new(-x, y, -z),
            new(-x, -y, z),  new(x, -y, z),  new(x, y, z),  new(-x, y, z),
        };
        int[][] faces = {
            new[]{0,1,2,3}, new[]{5,4,7,6}, new[]{4,0,3,7},
            new[]{1,5,6,2}, new[]{4,5,1,0}, new[]{3,2,6,7},
        };
        foreach (var f in faces)
        {
            int s = mesh.Positions.Count;
            mesh.Positions.Add(p[f[0]]); mesh.Positions.Add(p[f[1]]);
            mesh.Positions.Add(p[f[2]]); mesh.Positions.Add(p[f[3]]);
            mesh.TriangleIndices.Add(s+0); mesh.TriangleIndices.Add(s+1); mesh.TriangleIndices.Add(s+2);
            mesh.TriangleIndices.Add(s+0); mesh.TriangleIndices.Add(s+2); mesh.TriangleIndices.Add(s+3);
        }
        return mesh;
    }

    private static ModelVisual3D AxisLine(Point3D origin, Vector3D dir, Color color, double length)
    {
        var mesh = new MeshGeometry3D();
        var end = origin + dir * length;
        double w = 0.012;
        var side = Vector3D.CrossProduct(dir, new Vector3D(0, 1, 0));
        if (side.Length < 0.01) side = new Vector3D(1, 0, 0);
        side.Normalize();
        var up = Vector3D.CrossProduct(dir, side); up.Normalize();
        side *= w; up *= w;
        Point3D[] pts = {
            origin + side + up, origin - side + up, origin - side - up, origin + side - up,
            end + side + up, end - side + up, end - side - up, end + side - up,
        };
        foreach (var pt in pts) mesh.Positions.Add(pt);
        int[][] quads = { new[]{0,1,2,3}, new[]{4,5,6,7}, new[]{0,4,7,3}, new[]{1,5,6,2} };
        foreach (var q in quads)
        {
            mesh.TriangleIndices.Add(q[0]); mesh.TriangleIndices.Add(q[1]); mesh.TriangleIndices.Add(q[2]);
            mesh.TriangleIndices.Add(q[0]); mesh.TriangleIndices.Add(q[2]); mesh.TriangleIndices.Add(q[3]);
        }
        return new ModelVisual3D {
            Content = new GeometryModel3D(mesh, MaterialFrom(color)) { BackMaterial = MaterialFrom(color) },
        };
    }
}
