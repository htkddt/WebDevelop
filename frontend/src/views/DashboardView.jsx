import '../styles/Contents.css';
import '../styles/Dashboard.css';

export const DashboardView = ({ stats, loading, connected }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  const serverStatus = connected
    ? "🟢"
    : "🔴";

  return (
    <div className="dashboard-container">
      <div className="dashboard-header">
        <h1>
          System overview |
        </h1>
        <h2>
          Server status: {serverStatus}
        </h2>
      </div>
      <div className="dashboard-stats-grid">
        <div className="dashboard-stat-card">
          <h3>Total staff</h3>
          <p className="dashboard-stat-number">{stats.totalEmployees}</p>
        </div>
        <div className="dashboard-stat-card">
          <h3>Leave application awaiting approval</h3>
          <p className="dashboard-stat-number dashboard-color-warning">{stats.pendingLeave}</p>
        </div>
        <div className="dashboard-stat-card">
          <h3>Project is running</h3>
          <p className="dashboard-stat-number dashboard-color-success">{stats.activeProjects}</p>
        </div>
      </div>
    </div >
  );
};

export default DashboardView;