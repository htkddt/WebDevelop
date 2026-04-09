import '../styles/Contents.css';
import '../styles/Dashboard.css';

export const DashboardView = ({ stats, loading }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  return (
    <div className="dashboard-container">
      <h1 style={{ fontSize: '5rem', color: '#ccc' }}>
        System overview
      </h1>
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
    </div>
  );
};

export default DashboardView;